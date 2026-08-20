// 点云下行通道：订阅感知主机的 ROS 话题，降采样后以二进制帧推给遥控端。
//
// 按需启停。没人看的时候完全不连感知主机 —— 机器狗是生产设备，
// 不该因为我们挂了个订阅者就一直多一路 30 MB/s 的 TCP 流。
//
// 启停由一个独立的管理线程执行，不在 WebSocket 消息回调里做：
// RosClient::Stop() 要 join 订阅线程，最坏要等一个收超时，
// 卡在消息回调里会连带拖住控制指令。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "x30/point_cloud.hpp"
#include "x30/ros_client.hpp"
#include "x30/ws_server.hpp"

namespace x30 {

struct CloudBridgeConfig {
  std::string master_uri = "http://192.168.1.105:11311";

  // 本机在 ROS 网络里的地址，必须是感知主机能反连的那个 —— 也就是与狗直连的
  // eth0 地址，不是 MESH 侧的 eth1。填成 eth1 的地址会注册成功但收不到数据。
  std::string node_host = "192.168.1.120";

  // 机身合并云：扫描定位和 LIO 未就绪时的下行。
  std::string topic = "/lidar_points";

  // LIO 配准后的世界系当前扫描。有它时优先下行，轨迹才能和点云对上。
  std::string registered_topic = "/cloud_registered";

  PointCloudConfig cloud;
};

class CloudBridge {
 public:
  CloudBridge(CloudBridgeConfig config, WsServer& server);
  ~CloudBridge();

  CloudBridge(const CloudBridge&) = delete;
  CloudBridge& operator=(const CloudBridge&) = delete;

  void Start();
  void Stop();

  // 遥控端订阅/退订。空订阅者时会自动断开与感知主机的连接。
  void AddSubscriber(WsServer::ClientId id);
  void RemoveSubscriber(WsServer::ClientId id);

  // 每帧解析后回调（10 Hz 原云，抽帧之前）。只喂机体系，给扫描定位用。
  void SetFrameHandler(std::function<void(const PointCloudFrame&)> handler);

  // LIO 位姿，给世界系点云做距离裁剪（相对狗，而不是世界原点）。
  void SetWorldPose(float x, float y);

  // 供遥测里带上，遥控端据此显示"感知主机未连通"之类的提示。
  std::string StatusJson() const;

 private:
  void Supervisor();
  void OnBodyCloud(const uint8_t* data, size_t len);
  void OnWorldCloud(const uint8_t* data, size_t len);
  void Emit(PointCloudFrame* frame);
  bool WorldFresh(uint64_t now_ms) const;
  void StartRos();
  void StopRos();

  CloudBridgeConfig cfg_;
  WsServer& server_;
  std::function<void(const PointCloudFrame&)> frame_handler_;

  std::unique_ptr<RosClient> ros_;
  PointCloudEncoder encoder_;

  mutable std::mutex mutex_;
  std::set<WsServer::ClientId> subscribers_;
  std::string last_error_;
  uint64_t frames_sent_ = 0;
  uint64_t frames_dropped_ = 0;
  uint32_t last_points_ = 0;
  float last_voxel_ = 0.0f;
  bool last_world_ = false;
  float world_x_ = 0.0f;
  float world_y_ = 0.0f;
  uint64_t last_world_ms_ = 0;

  std::atomic<bool> running_{false};
  std::atomic<bool> ros_active_{false};
  std::thread supervisor_;
  std::condition_variable wake_;
  std::mutex wake_mutex_;

  // 复用编码缓冲，避免每帧重新分配上百 KB。
  std::vector<uint8_t> encode_buffer_;
  PointCloudFrame parse_buffer_;
};

}  // namespace x30
