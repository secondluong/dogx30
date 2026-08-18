#include "x30/udp_endpoint.hpp"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

namespace x30 {
namespace {

std::string LastSocketError() {
#if defined(_WIN32)
  return "WSA error " + std::to_string(WSAGetLastError());
#else
  return std::strerror(errno);
#endif
}

#if defined(_WIN32)
// Winsock 需要进程级初始化。用静态对象保证只做一次。
struct WsaGuard {
  WsaGuard() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WsaGuard() { WSACleanup(); }
};
void EnsureWsa() { static WsaGuard guard; }
#else
void EnsureWsa() {}
#endif

}  // namespace

struct UdpEndpoint::Impl {
  socket_t fd = kInvalidSocket;
  sockaddr_in peer{};
  bool has_peer = false;
};

UdpEndpoint::UdpEndpoint() : impl_(new Impl) { EnsureWsa(); }

UdpEndpoint::~UdpEndpoint() {
  Close();
  delete impl_;
}

bool UdpEndpoint::Open(uint16_t local_port, std::string* error) {
  impl_->fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (impl_->fd == kInvalidSocket) {
    if (error) *error = "创建套接字失败: " + LastSocketError();
    return false;
  }

  int reuse = 1;
  ::setsockopt(impl_->fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(local_port);
  if (::bind(impl_->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) {
      *error = "绑定本地端口 " + std::to_string(local_port) +
               " 失败: " + LastSocketError();
    }
    Close();
    return false;
  }
  return true;
}

bool UdpEndpoint::SetPeer(const std::string& ip, uint16_t port,
                          std::string* error) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    if (error) *error = "非法的 IP 地址: " + ip;
    return false;
  }
  impl_->peer = addr;
  impl_->has_peer = true;
  return true;
}

bool UdpEndpoint::Send(const void* data, size_t len) {
  if (impl_->fd == kInvalidSocket || !impl_->has_peer) return false;
  const auto sent =
      ::sendto(impl_->fd, static_cast<const char*>(data), static_cast<int>(len),
               0, reinterpret_cast<const sockaddr*>(&impl_->peer),
               sizeof(impl_->peer));
  return sent >= 0 && static_cast<size_t>(sent) == len;
}

int UdpEndpoint::Recv(void* buffer, size_t capacity, int timeout_ms) {
  if (impl_->fd == kInvalidSocket) return -1;

  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(impl_->fd, &read_set);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  const int ready =
      ::select(static_cast<int>(impl_->fd) + 1, &read_set, nullptr, nullptr, &tv);
  if (ready < 0) return -1;
  if (ready == 0) return 0;

  const auto received =
      ::recvfrom(impl_->fd, static_cast<char*>(buffer),
                 static_cast<int>(capacity), 0, nullptr, nullptr);
  return static_cast<int>(received);
}

void UdpEndpoint::Close() {
  if (impl_->fd == kInvalidSocket) return;
#if defined(_WIN32)
  ::closesocket(impl_->fd);
#else
  ::close(impl_->fd);
#endif
  impl_->fd = kInvalidSocket;
}

}  // namespace x30
