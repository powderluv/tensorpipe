/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tensorpipe/transport/ibv/context_impl.h>

#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <climits>

#include <tensorpipe/common/epoll_loop.h>
#include <tensorpipe/common/ibv.h>
#include <tensorpipe/common/socket.h>
#include <tensorpipe/common/system.h>
#include <tensorpipe/transport/ibv/connection_impl.h>
#include <tensorpipe/transport/ibv/error.h>
#include <tensorpipe/transport/ibv/listener_impl.h>
#include <tensorpipe/transport/ibv/reactor.h>
#include <tensorpipe/transport/ibv/sockaddr.h>

namespace tensorpipe {
namespace transport {
namespace ibv {

namespace {

// Prepend descriptor with transport name so it's easy to
// disambiguate descriptors when debugging.
const std::string kDomainDescriptorPrefix{"ibv:"};

std::string generateDomainDescriptor() {
  // It would be very cool if we could somehow obtain an "identifier" for the
  // InfiniBand subnet that our device belongs to, but nothing of that sort
  // seems to be available. So instead we say that if the user is trying to
  // connect two processes which both have access to an InfiniBand device then
  // they must know what they are doing and probably must have set up things
  // properly.
  return kDomainDescriptorPrefix + "*";
}

struct InterfaceAddressesDeleter {
  void operator()(struct ifaddrs* ptr) {
    ::freeifaddrs(ptr);
  }
};

using InterfaceAddresses =
    std::unique_ptr<struct ifaddrs, InterfaceAddressesDeleter>;

std::tuple<Error, InterfaceAddresses> createInterfaceAddresses() {
  struct ifaddrs* ifaddrs;
  auto rv = ::getifaddrs(&ifaddrs);
  if (rv < 0) {
    return std::make_tuple(
        TP_CREATE_ERROR(SystemError, "getifaddrs", errno),
        InterfaceAddresses());
  }
  return std::make_tuple(Error::kSuccess, InterfaceAddresses(ifaddrs));
}

std::tuple<Error, std::string> getHostname() {
  std::array<char, HOST_NAME_MAX> hostname;
  auto rv = ::gethostname(hostname.data(), hostname.size());
  if (rv < 0) {
    return std::make_tuple(
        TP_CREATE_ERROR(SystemError, "gethostname", errno), std::string());
  }
  return std::make_tuple(Error::kSuccess, std::string(hostname.data()));
}

struct AddressInfoDeleter {
  void operator()(struct addrinfo* ptr) {
    ::freeaddrinfo(ptr);
  }
};

using AddressInfo = std::unique_ptr<struct addrinfo, AddressInfoDeleter>;

std::tuple<Error, AddressInfo> createAddressInfo(std::string host) {
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* result;
  auto rv = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
  if (rv != 0) {
    return std::make_tuple(
        TP_CREATE_ERROR(GetaddrinfoError, rv), AddressInfo());
  }
  return std::make_tuple(Error::kSuccess, AddressInfo(result));
}

} // namespace

std::shared_ptr<ContextImpl> ContextImpl::create() {
  Error error;
  IbvLib ibvLib;
  std::tie(error, ibvLib) = IbvLib::create();
  if (error) {
    TP_VLOG(7)
        << "IBV transport is not viable because libibverbs couldn't be loaded: "
        << error.what();
    return std::make_shared<ContextImpl>();
  }

  IbvDeviceList deviceList;
  std::tie(error, deviceList) = IbvDeviceList::create(ibvLib);
  if (error && error.isOfType<SystemError>() &&
      error.castToType<SystemError>()->errorCode() == ENOSYS) {
    TP_VLOG(7) << "IBV transport is not viable because it couldn't get list of "
               << "InfiniBand devices because the kernel module isn't loaded";
    return std::make_shared<ContextImpl>();
  }
  TP_THROW_ASSERT_IF(error)
      << "Couldn't get list of InfiniBand devices: " << error.what();

  if (deviceList.size() == 0) {
    TP_VLOG(7) << "IBV transport is not viable because it couldn't find any "
               << "InfiniBand NICs";
    return std::make_shared<ContextImpl>();
  }

  return std::make_shared<ContextImpl>(
      std::move(ibvLib), std::move(deviceList));
}

ContextImpl::ContextImpl()
    : ContextImplBoilerplate<ContextImpl, ListenerImpl, ConnectionImpl>(
          /*domainDescriptor=*/""),
      isViable_(false) {}

ContextImpl::ContextImpl(IbvLib ibvLib, IbvDeviceList deviceList)
    : ContextImplBoilerplate<ContextImpl, ListenerImpl, ConnectionImpl>(
          generateDomainDescriptor()),
      isViable_(true),
      reactor_(std::move(ibvLib), std::move(deviceList)) {}

void ContextImpl::closeImpl() {
  loop_.close();
  reactor_.close();
}

void ContextImpl::joinImpl() {
  loop_.join();
  reactor_.join();
}

bool ContextImpl::isViable() const {
  return isViable_;
}

std::tuple<Error, std::string> ContextImpl::lookupAddrForIface(
    std::string iface) {
  Error error;
  InterfaceAddresses addresses;
  std::tie(error, addresses) = createInterfaceAddresses();
  if (error) {
    return std::make_tuple(std::move(error), std::string());
  }

  struct ifaddrs* ifa;
  for (ifa = addresses.get(); ifa != nullptr; ifa = ifa->ifa_next) {
    // Skip entry if ifa_addr is NULL (see getifaddrs(3))
    if (ifa->ifa_addr == nullptr) {
      continue;
    }

    if (iface != ifa->ifa_name) {
      continue;
    }

    switch (ifa->ifa_addr->sa_family) {
      case AF_INET:
        return std::make_tuple(
            Error::kSuccess,
            Sockaddr(ifa->ifa_addr, sizeof(struct sockaddr_in)).str());
      case AF_INET6:
        return std::make_tuple(
            Error::kSuccess,
            Sockaddr(ifa->ifa_addr, sizeof(struct sockaddr_in6)).str());
    }
  }

  return std::make_tuple(TP_CREATE_ERROR(NoAddrFoundError), std::string());
}

std::tuple<Error, std::string> ContextImpl::lookupAddrForHostname() {
  Error error;
  std::string hostname;
  std::tie(error, hostname) = getHostname();
  if (error) {
    return std::make_tuple(std::move(error), std::string());
  }

  AddressInfo info;
  std::tie(error, info) = createAddressInfo(std::move(hostname));
  if (error) {
    return std::make_tuple(std::move(error), std::string());
  }

  Error firstError;
  for (struct addrinfo* rp = info.get(); rp != nullptr; rp = rp->ai_next) {
    TP_DCHECK(rp->ai_family == AF_INET || rp->ai_family == AF_INET6);
    TP_DCHECK_EQ(rp->ai_socktype, SOCK_STREAM);
    TP_DCHECK_EQ(rp->ai_protocol, IPPROTO_TCP);

    Sockaddr addr = Sockaddr(rp->ai_addr, rp->ai_addrlen);

    Socket socket;
    std::tie(error, socket) = Socket::createForFamily(rp->ai_family);

    if (!error) {
      error = socket.bind(addr);
    }

    if (error) {
      // Record the first binding error we encounter and return that in the end
      // if no working address is found, in order to help with debugging.
      if (!firstError) {
        firstError = error;
      }
      continue;
    }

    return std::make_tuple(Error::kSuccess, addr.str());
  }

  if (firstError) {
    return std::make_tuple(std::move(firstError), std::string());
  } else {
    return std::make_tuple(TP_CREATE_ERROR(NoAddrFoundError), std::string());
  }
}

bool ContextImpl::inLoop() const {
  return reactor_.inLoop();
};

void ContextImpl::deferToLoop(std::function<void()> fn) {
  reactor_.deferToLoop(std::move(fn));
};

void ContextImpl::registerDescriptor(
    int fd,
    int events,
    std::shared_ptr<EpollLoop::EventHandler> h) {
  loop_.registerDescriptor(fd, events, std::move(h));
}

void ContextImpl::unregisterDescriptor(int fd) {
  loop_.unregisterDescriptor(fd);
}

Reactor& ContextImpl::getReactor() {
  return reactor_;
}

} // namespace ibv
} // namespace transport
} // namespace tensorpipe
