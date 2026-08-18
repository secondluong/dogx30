#include "x30/net_util.hpp"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t_compat = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_t_compat = socklen_t;
#define closesocket ::close
#endif

namespace x30 {
namespace {

bool ConnectWithTimeout(int fd, const sockaddr* addr,
                        socklen_t_compat addrlen, int timeout_ms) {
#if defined(_WIN32)
  u_long nonblock = 1;
  ::ioctlsocket(fd, FIONBIO, &nonblock);
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

  bool ok = false;
  if (::connect(fd, addr, addrlen) == 0) {
    ok = true;
  } else {
#if defined(_WIN32)
    const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
    const bool pending = errno == EINPROGRESS;
#endif
    if (pending) {
      fd_set write_set;
      FD_ZERO(&write_set);
      FD_SET(fd, &write_set);
      timeval tv{};
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      if (::select(fd + 1, nullptr, &write_set, nullptr, &tv) > 0) {
        int err = 0;
        socklen_t_compat len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&err), &len) == 0 &&
            err == 0) {
          ok = true;
        }
      }
    }
  }

#if defined(_WIN32)
  nonblock = 0;
  ::ioctlsocket(fd, FIONBIO, &nonblock);
#else
  ::fcntl(fd, F_SETFL, flags);
#endif
  return ok;
}

}  // namespace

int TcpConnectTimeout(const std::string& host, uint16_t port,
                      int connect_timeout_ms, int recv_timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(port);
  if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 ||
      res == nullptr) {
    return -1;
  }

  const int fd = static_cast<int>(
      ::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
  if (fd < 0) {
    ::freeaddrinfo(res);
    return -1;
  }

  if (!ConnectWithTimeout(fd, res->ai_addr,
                          static_cast<socklen_t_compat>(res->ai_addrlen),
                          connect_timeout_ms)) {
    ::freeaddrinfo(res);
    closesocket(fd);
    return -1;
  }
  ::freeaddrinfo(res);

#if defined(_WIN32)
  DWORD tv = static_cast<DWORD>(recv_timeout_ms);
#else
  timeval tv{};
  tv.tv_sec = recv_timeout_ms / 1000;
  tv.tv_usec = (recv_timeout_ms % 1000) * 1000;
#endif
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));

  return fd;
}

}  // namespace x30
