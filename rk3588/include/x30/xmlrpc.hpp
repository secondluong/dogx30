// 够用就好的 XML-RPC。只覆盖 ROS1 主从节点 API 用到的子集。
//
// 为什么自己写：ROS1 Noetic 官方只支持 Ubuntu 20.04，而板子上跑的是 22.04
// （选 22.04 的理由见 rk3588-setup.md：MPP 与较新的编译器）。在 jammy 上装
// Noetic 要么源码编译整套要么上 conda，都很脏。而我们只需要「订阅两三个话题」
// 这一点点能力，直接说 ROS 的线上协议反而干净 —— 这和项目里自己写 WebSocket、
// 自己写 UDP 协议是同一个取舍。
//
// XML-RPC 本身很简单：HTTP POST 一段 XML，回一段 XML。ROS 用到的值类型只有
// int / string / array，不需要通用实现。

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace x30 {

// XML-RPC 值。ROS 的主从 API 只用到这几种。
struct XmlRpcValue {
  enum class Type { kInt, kString, kArray };

  Type type = Type::kString;
  int int_value = 0;
  std::string str_value;
  std::vector<XmlRpcValue> array;

  static XmlRpcValue Int(int v);
  static XmlRpcValue Str(std::string v);
  static XmlRpcValue Array(std::vector<XmlRpcValue> v);

  // 数组取元素，越界返回空值。ROS 的返回都是 [code, statusMsg, value] 三元组，
  // 到处判长度太啰嗦。
  const XmlRpcValue& At(size_t i) const;
  int AsInt() const { return int_value; }
  const std::string& AsString() const { return str_value; }
  size_t Size() const { return array.size(); }

  std::string ToXml() const;
};

// 同步调用一次 XML-RPC。uri 形如 http://192.168.1.105:11311/。
// 失败返回 false 并填 error；成功时 result 是 <methodResponse> 里的那个值。
bool XmlRpcCall(const std::string& uri, const std::string& method,
                const std::vector<XmlRpcValue>& params, XmlRpcValue* result,
                std::string* error, int timeout_ms = 3000);

// 解析 <methodCall>，供从节点服务端使用。
bool ParseMethodCall(const std::string& xml, std::string* method,
                     std::vector<XmlRpcValue>* params);

// 最小 XML-RPC 服务端。ROS 的 master 会回调从节点的 publisherUpdate，
// 不实现的话 master 日志里会一直刷错误 —— 机器狗是生产设备，不该被我们污染。
class XmlRpcServer {
 public:
  using Handler = std::function<XmlRpcValue(const std::vector<XmlRpcValue>&)>;

  XmlRpcServer();
  ~XmlRpcServer();

  XmlRpcServer(const XmlRpcServer&) = delete;
  XmlRpcServer& operator=(const XmlRpcServer&) = delete;

  void On(const std::string& method, Handler handler);

  // port 传 0 表示由系统分配，之后用 port() 取实际端口。
  bool Start(const std::string& bind_address, uint16_t port,
             std::string* error);
  void Stop();

  uint16_t port() const { return port_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint16_t port_ = 0;
};

}  // namespace x30
