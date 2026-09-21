// 极薄的 UDP 套接字封装。目标平台是 RK3588 上的 Linux，同时兼容 Windows
// 以便在开发机上配合仿真器联调。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace x30 {

class UdpEndpoint {
 public:
  UdpEndpoint();
  ~UdpEndpoint();

  UdpEndpoint(const UdpEndpoint&) = delete;
  UdpEndpoint& operator=(const UdpEndpoint&) = delete;

  // 绑定本地端口用于接收遥测。local_port 为 0 时由系统分配（此时收不到
  // 运动主机的单播遥测，因为 network.toml 里登记的是固定端口）。
  // local_ip 非空时绑到该地址，避免双地址网卡用 10.2 去打狗。
  bool Open(uint16_t local_port, std::string* error);
  bool Open(uint16_t local_port, const std::string& local_ip, std::string* error);

  // 设置默认发送目标，即运动主机 192.168.1.103:43893。
  bool SetPeer(const std::string& ip, uint16_t port, std::string* error);

  bool Send(const void* data, size_t len);

  // 发到指定地址，不改默认 peer。载荷开关回源、灯板 :9000 都走这条。
  bool SendTo(const std::string& ip, uint16_t port, const void* data,
              size_t len);

  // 阻塞接收，超时返回 0，出错返回 -1。
  int Recv(void* buffer, size_t capacity, int timeout_ms);

  // 同上，并带回源地址。src_ip / src_port 可为空。
  int RecvFrom(void* buffer, size_t capacity, int timeout_ms,
               std::string* src_ip, uint16_t* src_port);

  void Close();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace x30
