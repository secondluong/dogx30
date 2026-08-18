// ROS1 话题订阅，不依赖 ROS 安装。
//
// 走的是 ROS 的线上协议：XML-RPC 找到发布者，再用 TCPROS 直连收消息。
// 板子上不需要装 roscpp，也就绕开了「Noetic 只支持 20.04 而我们跑 22.04」
// 这个死结。代价是消息类型要手工对齐 md5sum，收益是零依赖。
//
// 只做订阅，不做发布和服务 —— 我们对机器人本体是只读的，
// 运动指令一律走已有的 UDP 通道，不从 ROS 侧下发。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "x30/xmlrpc.hpp"

namespace x30 {

// 已知消息类型的 md5。填错会被发布者直接拒连并打印类型不匹配，
// 所以这几个值是查证过的，不要凭印象改。
namespace ros_md5 {
inline constexpr const char* kPointCloud2 = "1158d486dd51d683ce2f1be655c3c181";
inline constexpr const char* kImu = "6a62c6daae103f4ff57a132d6f95cec2";
inline constexpr const char* kOdometry = "cd5e73d190d741a2f92e81eda573aca7";
}  // namespace ros_md5

struct RosTopic {
  std::string name;      // "/lidar_points"
  std::string type;      // "sensor_msgs/PointCloud2"
  std::string md5;       // 见 ros_md5
};

struct RosClientConfig {
  std::string master_uri = "http://192.168.1.105:11311";

  // 本机在 ROS 网络里的地址。**必须是感知主机能反向连到的地址**，
  // 填错的症状是注册成功但一条消息都收不到。
  std::string node_host = "192.168.1.120";

  std::string node_name = "/x30_gateway";

  // 重连间隔。感知主机重启或话题短暂消失时，不要疯狂重试打爆它。
  int retry_ms = 2000;
};

// 收到一条消息的回调。data 指向反序列化前的原始消息体（不含 4 字节长度前缀），
// 只在回调期间有效，要留就自己拷。回调在订阅线程上执行，不要在里面做重活。
using RosMessageHandler =
    std::function<void(const uint8_t* data, size_t len)>;

class RosClient {
 public:
  explicit RosClient(RosClientConfig config);
  ~RosClient();

  RosClient(const RosClient&) = delete;
  RosClient& operator=(const RosClient&) = delete;

  // 登记一个订阅。必须在 Start 之前调用。
  void Subscribe(const RosTopic& topic, RosMessageHandler handler);

  bool Start(std::string* error);
  void Stop();

  // 是否至少有一个话题连上了。遥控端据此显示「感知主机未连通」。
  bool connected() const { return connected_.load(); }

  // 最近一次失败原因，供界面和日志展示。可达性是这条链路最大的不确定性，
  // 出问题时要能一眼看出卡在哪一步。
  std::string last_error() const;

 private:
  struct Subscription;

  void SubscriptionLoop(Subscription* sub);
  bool ConnectOnce(Subscription* sub, std::string* error);
  void SetError(const std::string& e);

  RosClientConfig cfg_;
  XmlRpcServer slave_;
  std::string node_uri_;

  std::vector<std::unique_ptr<Subscription>> subs_;
  std::vector<std::thread> threads_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};

  mutable std::mutex error_mutex_;
  std::string last_error_;
};

}  // namespace x30
