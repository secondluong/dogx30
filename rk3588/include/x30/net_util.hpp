// 网络小工具。目前只有一件事：带超时的 TCP 连接。

#pragma once

#include <cstdint>
#include <string>

namespace x30 {

// 连到 host:port，超时返回 -1，成功返回已就绪的阻塞式套接字。
//
// 存在的理由是标准 connect() 没法设超时：SO_SNDTIMEO 管不到它。
// 对端掉电时内核会重传 SYN 一分多钟，而调用方（ROS 发现、XML-RPC）都在
// 网关的启停路径上 —— 感知主机没上电时，关一次网关要等两分钟。
//
// recv_timeout_ms 同时设到 SO_RCVTIMEO 上，让后续的读也不会无限阻塞；
// 收发线程靠它周期性回到循环顶部检查停止标志。
int TcpConnectTimeout(const std::string& host, uint16_t port,
                      int connect_timeout_ms, int recv_timeout_ms);

}  // namespace x30
