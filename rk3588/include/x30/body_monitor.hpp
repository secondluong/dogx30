#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace x30 {

// 官方《本体监控协议》Type=1002 的基础状态。该接口由智能控制器
// 192.168.1.106:30000 汇总运动、感知和充电状态，语义比运动主机的原始
// basic_state 完整（额外区分摔倒=7、RL=16）。
struct BodyMonitorState {
  bool alive = false;
  int motion_state = -1;
  int gait_state = -1;
  int motor_state = -1;
  int charge_state = -1;
  int control_mode = -1;
  int on_dock_state = -1;
  int location_state = -1;
  int electricity = -1;
  float speed = 0.0f;
};

struct BodyMonitorConfig {
  std::string host = "192.168.1.106";
  uint16_t port = 30000;
  int poll_ms = 200;
  int connect_timeout_ms = 500;
  int receive_timeout_ms = 700;
};

class BodyMonitor {
 public:
  using StateHandler = std::function<void(const BodyMonitorState&)>;

  explicit BodyMonitor(BodyMonitorConfig config);
  ~BodyMonitor();

  BodyMonitor(const BodyMonitor&) = delete;
  BodyMonitor& operator=(const BodyMonitor&) = delete;

  void SetHandler(StateHandler handler);
  void Start();
  void Stop();
  BodyMonitorState Snapshot() const;

 private:
  void Loop();
  bool Poll(uint16_t request_id, BodyMonitorState* out);

  BodyMonitorConfig cfg_;
  mutable std::mutex mutex_;
  BodyMonitorState state_;
  StateHandler handler_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace x30
