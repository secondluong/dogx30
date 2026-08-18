// 把 WebSocket 协议接到 MotionClient 上。
//
// 三件事：解析并分发遥控端指令、按固定频率推送遥测、仲裁控制权。
// 协议定义见 docs/app-protocol.md。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <memory>

#include "x30/cloud_bridge.hpp"
#include "x30/gait_coordinator.hpp"
#include "x30/media_registry.hpp"
#include "x30/motion_client.hpp"
#include "x30/terrain_client.hpp"
#include "x30/ws_server.hpp"

namespace x30 {

struct RobotServiceConfig {
  uint16_t port = 8080;
  std::string static_root = "web";

  // 监听地址。协议本身没有身份认证，任何能连上这个端口的人都能申请控制权，
  // 所以装了 4G 等广域接口时务必绑定到遥控链路所在的地址，不要留 0.0.0.0。
  std::string bind_address = "0.0.0.0";

  // 遥测推送频率。
  int state_rate_hz = 10;

  // 媒体源配置文件。为空或文件不存在时视为没有视频，控制功能不受影响 ——
  // 视频是附加能力，不该因为它缺失就让整个网关起不来。
  std::string media_config;

  // 点云桥接。默认关闭：感知主机的 ROS 是否可达尚未现场验证，
  // 没验证过的东西不该在开机时就去连一台生产设备。
  bool cloud_enabled = false;
  CloudBridgeConfig cloud;

  // 控制权租约：持有者这么久没发任何消息就自动释放，供其他客户端接管。
  // 遥控端应以 2 Hz 发心跳，这个值留了 6 倍余量。
  // 它兜的是 TCP 半开连接 —— 平板突然断电时 OnDisconnect 可能迟迟不触发。
  int control_lease_ms = 3000;
};

class RobotService {
 public:
  RobotService(MotionClient& client, TerrainClient& terrain,
               RobotServiceConfig config);
  ~RobotService();

  RobotService(const RobotService&) = delete;
  RobotService& operator=(const RobotService&) = delete;

  bool Start(std::string* error);
  void Stop();

 private:
  void OnConnect(WsServer::ClientId id);
  void OnMessage(WsServer::ClientId id, const std::string& text);
  void OnDisconnect(WsServer::ClientId id);
  void StateLoop();

  // 控制权。返回 true 表示 id 当前有权下发控制指令。
  bool HoldsControl(WsServer::ClientId id);
  bool TryClaim(WsServer::ClientId id);
  void ReleaseControl(WsServer::ClientId id, bool zero_axes);
  void BroadcastControlState();

  // 持有者的任意一条消息都续租。租约衡量的是"遥控端还活着"，不是"正在推摇杆"——
  // 狗站起来的几秒里遥控端本来就没有摇杆量要发，不该因此被剥夺控制权。
  void TouchLease(WsServer::ClientId id);

  void SendError(WsServer::ClientId id, const char* code, const char* msg);
  void SendGaitResult(WsServer::ClientId id, Gait target,
                      const GaitCoordinator::Result& result);
  void SendMediaPlan(WsServer::ClientId id);

  std::string BuildStateJson() const;

  MotionClient& client_;
  TerrainClient& terrain_;
  RobotServiceConfig cfg_;
  WsServer server_;
  GaitCoordinator gaits_;

  // 没配媒体源时为空，所有媒体消息一律回「未配置」。
  std::unique_ptr<MediaRegistry> media_;

  // 未开启点云时为空，cloud_* 消息一律回「未启用」。
  std::unique_ptr<CloudBridge> cloud_;

  std::thread state_thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex control_mutex_;
  WsServer::ClientId controller_ = 0;  // 0 = 无人持有
  std::chrono::steady_clock::time_point lease_expiry_{};
};

}  // namespace x30
