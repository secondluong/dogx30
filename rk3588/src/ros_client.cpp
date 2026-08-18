#include "x30/ros_client.hpp"

#include "x30/net_util.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t_compat = int;
using ssize_t_compat = int;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_t_compat = socklen_t;
using ssize_t_compat = ssize_t;
#define closesocket ::close
#endif

namespace x30 {
namespace {

void PutU32(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
}

// TCPROS 的连接头是一串 "长度 + key=value"，整体再套一个总长度。
void AppendField(std::string* out, const std::string& kv) {
  PutU32(out, static_cast<uint32_t>(kv.size()));
  *out += kv;
}

bool SendAll(int fd, const char* data, size_t len) {
  while (len > 0) {
    const ssize_t_compat n = ::send(fd, data, static_cast<int>(len), 0);
    if (n <= 0) return false;
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool RecvExact(int fd, uint8_t* buf, size_t len,
               const std::atomic<bool>& running) {
  size_t got = 0;
  while (got < len) {
    if (!running.load()) return false;
    const ssize_t_compat n =
        ::recv(fd, reinterpret_cast<char*>(buf + got),
               static_cast<int>(len - got), 0);
    if (n == 0) return false;
    if (n < 0) {
#if !defined(_WIN32)
      if (errno == EINTR) continue;
      // 收超时是正常的：话题可能本来就低频，用它来周期性检查 running。
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
      return false;
    }
    got += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

struct RosClient::Subscription {
  RosTopic topic;
  RosMessageHandler handler;
  std::atomic<int> fd{-1};
  std::atomic<bool> connected{false};
};

RosClient::RosClient(RosClientConfig config) : cfg_(std::move(config)) {}

RosClient::~RosClient() { Stop(); }

void RosClient::Subscribe(const RosTopic& topic, RosMessageHandler handler) {
  auto sub = std::unique_ptr<Subscription>(new Subscription);
  sub->topic = topic;
  sub->handler = std::move(handler);
  subs_.push_back(std::move(sub));
}

void RosClient::SetError(const std::string& e) {
  std::lock_guard<std::mutex> lock(error_mutex_);
  last_error_ = e;
}

std::string RosClient::last_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

bool RosClient::Start(std::string* error) {
  if (running_.load()) return true;
  if (subs_.empty()) {
    if (error) *error = "没有登记任何话题";
    return false;
  }

  // 从节点服务端。master 会回调 publisherUpdate 通知发布者变化；
  // 不实现的话 master 日志会一直刷错误，而机器狗是生产设备，不该被我们污染。
  slave_.On("getPid", [](const std::vector<XmlRpcValue>&) {
    return XmlRpcValue::Array({XmlRpcValue::Int(1), XmlRpcValue::Str("ok"),
                               XmlRpcValue::Int(0)});
  });
  slave_.On("publisherUpdate", [this](const std::vector<XmlRpcValue>&) {
    // 收到就把现有连接断掉，订阅线程会自己重新走一遍发现流程。
    // 比在回调里解析新列表简单得多，而这个事件很罕见，不值得优化。
    for (auto& sub : subs_) {
      const int fd = sub->fd.exchange(-1);
      if (fd >= 0) closesocket(fd);
    }
    return XmlRpcValue::Array({XmlRpcValue::Int(1), XmlRpcValue::Str("ok"),
                               XmlRpcValue::Int(0)});
  });
  slave_.On("getSubscriptions", [this](const std::vector<XmlRpcValue>&) {
    std::vector<XmlRpcValue> list;
    for (const auto& sub : subs_) {
      list.push_back(XmlRpcValue::Array({XmlRpcValue::Str(sub->topic.name),
                                         XmlRpcValue::Str(sub->topic.type)}));
    }
    return XmlRpcValue::Array({XmlRpcValue::Int(1), XmlRpcValue::Str("ok"),
                               XmlRpcValue::Array(std::move(list))});
  });
  slave_.On("getPublications", [](const std::vector<XmlRpcValue>&) {
    return XmlRpcValue::Array({XmlRpcValue::Int(1), XmlRpcValue::Str("ok"),
                               XmlRpcValue::Array({})});
  });

  std::string slave_err;
  if (!slave_.Start("0.0.0.0", 0, &slave_err)) {
    if (error) *error = "从节点服务启动失败: " + slave_err;
    return false;
  }
  node_uri_ = "http://" + cfg_.node_host + ":" +
              std::to_string(slave_.port()) + "/";

  running_.store(true);
  for (auto& sub : subs_) {
    threads_.emplace_back(&RosClient::SubscriptionLoop, this, sub.get());
  }
  return true;
}

void RosClient::Stop() {
  if (!running_.exchange(false)) return;
  for (auto& sub : subs_) {
    const int fd = sub->fd.exchange(-1);
    if (fd >= 0) closesocket(fd);
  }
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  threads_.clear();
  slave_.Stop();
  connected_.store(false);
}

// 一次完整的发现 + 握手。成功后 sub->fd 是可读的 TCPROS 连接。
bool RosClient::ConnectOnce(Subscription* sub, std::string* error) {
  // 1. 向 master 注册订阅，拿到当前发布者列表。
  XmlRpcValue reply;
  std::string rpc_err;
  const bool ok = XmlRpcCall(
      cfg_.master_uri, "registerSubscriber",
      {XmlRpcValue::Str(cfg_.node_name), XmlRpcValue::Str(sub->topic.name),
       XmlRpcValue::Str(sub->topic.type), XmlRpcValue::Str(node_uri_)},
      &reply, &rpc_err);
  if (!ok) {
    *error = "连不上 ROS master (" + cfg_.master_uri + "): " + rpc_err;
    return false;
  }
  if (reply.At(0).AsInt() != 1) {
    *error = "master 拒绝注册: " + reply.At(1).AsString();
    return false;
  }

  const XmlRpcValue& publishers = reply.At(2);
  if (publishers.Size() == 0) {
    *error = "话题 " + sub->topic.name + " 当前没有发布者";
    return false;
  }

  // 2. 问发布者要 TCPROS 的地址端口。
  const std::string pub_uri = publishers.At(0).AsString();
  XmlRpcValue topic_reply;
  if (!XmlRpcCall(pub_uri, "requestTopic",
                  {XmlRpcValue::Str(cfg_.node_name),
                   XmlRpcValue::Str(sub->topic.name),
                   XmlRpcValue::Array({XmlRpcValue::Array(
                       {XmlRpcValue::Str("TCPROS")})})},
                  &topic_reply, &rpc_err)) {
    *error = "requestTopic 失败 (" + pub_uri + "): " + rpc_err;
    return false;
  }
  if (topic_reply.At(0).AsInt() != 1) {
    *error = "发布者拒绝: " + topic_reply.At(1).AsString();
    return false;
  }

  const XmlRpcValue& proto = topic_reply.At(2);
  const std::string host = proto.At(1).AsString();
  const auto port = static_cast<uint16_t>(proto.At(2).AsInt());
  if (host.empty() || port == 0) {
    *error = "发布者返回的 TCPROS 地址无效";
    return false;
  }

  // 3. 连上去，发连接头。收超时设 1 秒，订阅线程靠它周期性检查停止标志。
  const int fd = TcpConnectTimeout(host, port, 2000, 1000);
  if (fd >= 0) {
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
  }
  if (fd < 0) {
    *error = "连接发布者 " + host + ":" + std::to_string(port) + " 失败";
    return false;
  }

  std::string fields;
  AppendField(&fields, "callerid=" + cfg_.node_name);
  AppendField(&fields, "topic=" + sub->topic.name);
  AppendField(&fields, "type=" + sub->topic.type);
  AppendField(&fields, "md5sum=" + sub->topic.md5);
  // 点云是大包，延迟不敏感；但 IMU 是小包高频，攒包会让时间戳失真。
  AppendField(&fields, "tcp_nodelay=1");

  std::string header;
  PutU32(&header, static_cast<uint32_t>(fields.size()));
  header += fields;

  if (!SendAll(fd, header.data(), header.size())) {
    closesocket(fd);
    *error = "发送连接头失败";
    return false;
  }

  // 4. 读回发布者的头。md5 不匹配时发布者会在这里回一个带 error= 的头然后断开。
  uint8_t len_buf[4];
  if (!RecvExact(fd, len_buf, 4, running_)) {
    closesocket(fd);
    *error = "发布者没有回应连接头（多半是 md5sum 不匹配）";
    return false;
  }
  const uint32_t reply_len = static_cast<uint32_t>(len_buf[0]) |
                             (static_cast<uint32_t>(len_buf[1]) << 8) |
                             (static_cast<uint32_t>(len_buf[2]) << 16) |
                             (static_cast<uint32_t>(len_buf[3]) << 24);
  if (reply_len > 64u * 1024) {
    closesocket(fd);
    *error = "连接头长度异常";
    return false;
  }
  std::vector<uint8_t> reply_buf(reply_len);
  if (reply_len > 0 && !RecvExact(fd, reply_buf.data(), reply_len, running_)) {
    closesocket(fd);
    *error = "读连接头失败";
    return false;
  }
  const std::string reply_text(
      reinterpret_cast<const char*>(reply_buf.data()), reply_buf.size());
  if (reply_text.find("error=") != std::string::npos) {
    closesocket(fd);
    *error = "发布者拒绝连接: " + reply_text;
    return false;
  }

  sub->fd.store(fd);
  return true;
}

void RosClient::SubscriptionLoop(Subscription* sub) {
  std::vector<uint8_t> buffer;

  while (running_.load()) {
    std::string error;
    if (!ConnectOnce(sub, &error)) {
      sub->connected.store(false);
      SetError(sub->topic.name + ": " + error);
      // 有一个话题连上就算连通，不要因为某个低频话题没起来就整体报断。
      bool any = false;
      for (const auto& s : subs_) {
        if (s->connected.load()) any = true;
      }
      connected_.store(any);

      for (int i = 0; i < cfg_.retry_ms / 100 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      continue;
    }

    sub->connected.store(true);
    connected_.store(true);
    std::printf("[ros] 已订阅 %s\n", sub->topic.name.c_str());

    // 消息循环：4 字节小端长度 + 消息体。
    while (running_.load()) {
      const int fd = sub->fd.load();
      if (fd < 0) break;

      uint8_t len_buf[4];
      if (!RecvExact(fd, len_buf, 4, running_)) break;
      const uint32_t len = static_cast<uint32_t>(len_buf[0]) |
                           (static_cast<uint32_t>(len_buf[1]) << 8) |
                           (static_cast<uint32_t>(len_buf[2]) << 16) |
                           (static_cast<uint32_t>(len_buf[3]) << 24);

      // 四台 Mid-360 的合并云一帧也就几 MB，超过这个量级说明流已经错位了，
      // 继续读只会把内存吃光。
      if (len > 32u * 1024 * 1024) {
        SetError(sub->topic.name + ": 消息长度异常，断开重连");
        break;
      }

      buffer.resize(len);
      if (len > 0 && !RecvExact(fd, buffer.data(), len, running_)) break;
      if (sub->handler) sub->handler(buffer.data(), len);
    }

    const int fd = sub->fd.exchange(-1);
    if (fd >= 0) closesocket(fd);
    sub->connected.store(false);
    if (running_.load()) {
      std::printf("[ros] %s 连接断开，准备重连\n", sub->topic.name.c_str());
    }
  }
}

}  // namespace x30
