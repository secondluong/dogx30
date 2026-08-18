// HTTP 静态文件 + WebSocket 服务器，零第三方依赖。
//
// 只实现 RFC 6455 里本项目用得到的部分：文本/二进制帧、分片重组、ping/pong、
// close 握手。没有 TLS、没有压缩扩展、没有 HTTP keep-alive 之外的花样。
// 遥控端在同一局域网内，不需要 TLS；真要加，前面挡一层 nginx 即可。
//
// 并发模型：一个 accept 线程 + 每连接一个线程。客户端数量是个位数（平板、
// 笔记本、调试浏览器），线程模型比 epoll 状态机简单得多，也更不容易出错。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace x30 {

class WsServer {
 public:
  using ClientId = uint64_t;
  using ConnectHandler = std::function<void(ClientId)>;
  using MessageHandler = std::function<void(ClientId, const std::string&)>;
  using DisconnectHandler = std::function<void(ClientId)>;

  WsServer();
  ~WsServer();

  WsServer(const WsServer&) = delete;
  WsServer& operator=(const WsServer&) = delete;

  // 静态文件根目录。为空则只提供 WebSocket。
  void SetStaticRoot(std::string dir) { static_root_ = std::move(dir); }

  void SetHandlers(ConnectHandler on_connect, MessageHandler on_message,
                   DisconnectHandler on_disconnect);

  // bind_address 为点分十进制。"0.0.0.0" 监听全部网卡；指定具体地址可把服务
  // 限制在单一链路上，避免经 4G 等广域接口暴露。
  bool Start(const std::string& bind_address, uint16_t port, std::string* error);
  void Stop();

  void Send(ClientId id, const std::string& text);
  void Broadcast(const std::string& text);

  // 二进制下行。点云走这条：同样的点数，二进制比 JSON 小一个数量级，
  // 而 MESH 只有十几 Mbps，用 JSON 传点云是不可能的。
  // 返回 false 表示发送队列已满并被丢弃，调用方据此降频而不是无限堆积。
  bool SendBinary(ClientId id, const void* data, size_t len);
  size_t ClientCount() const;

  // 当前在线客户端。用于需要给每个客户端发**不同内容**的场合（如媒体计划
  // 因人而异），这时 Broadcast 不适用。
  std::vector<ClientId> ClientIds() const;

 private:
  struct Connection;

  void AcceptLoop();
  void SessionLoop(std::shared_ptr<Connection> conn);
  bool ServeHttp(Connection& conn, const std::string& method,
                 const std::string& path);

  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::string static_root_;

  std::thread accept_thread_;
  std::atomic<bool> running_{false};
  std::atomic<ClientId> next_id_{1};

  mutable std::mutex clients_mutex_;
  std::map<ClientId, std::shared_ptr<Connection>> clients_;
  std::vector<std::thread> session_threads_;

  ConnectHandler on_connect_;
  MessageHandler on_message_;
  DisconnectHandler on_disconnect_;
};

}  // namespace x30
