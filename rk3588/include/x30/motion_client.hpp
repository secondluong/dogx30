// X30 运动控制客户端。
//
// 职责：
//   - 维持与运动主机的心跳，并在首次心跳后补发连接确认；
//   - 以 50 Hz 稳定下发轴指令（这是协议硬性要求，超过 1 秒不发即失效）；
//   - 接收并解析运动主机单播回来的遥测；
//   - 看门狗：上层（平板）一旦停止喂数据，立即把轴指令清零，让机器人原地踏步
//     而不是带着最后一次速度继续跑。
//
// 线程模型：Start() 拉起一个发送线程和一个接收线程，其余方法可从任意线程调用。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "x30/protocol.hpp"
#include "x30/udp_endpoint.hpp"

namespace x30 {

struct MotionClientConfig {
  std::string robot_ip = "192.168.1.103";
  uint16_t robot_port = 43893;

  // 本机接收遥测的端口。必须与运动主机 network.toml 中为本机登记的端口一致。
  uint16_t local_port = 43897;

  // 轴指令下发频率。协议要求 50 Hz。
  int axis_rate_hz = 50;

  // 心跳频率。协议要求 ≥2 Hz，留足余量。
  int heartbeat_rate_hz = 10;

  // 看门狗：距上次 SetVelocity/SetPose 超过这个时间就把轴指令清零。
  int command_timeout_ms = 300;

  // 超过这个时间没收到任何遥测就认为与机器人失联。
  int telemetry_timeout_ms = 1000;
};

// 遥测快照。由接收线程更新，调用方通过 Snapshot() 取一份一致的副本。
struct RobotState {
  bool telemetry_alive = false;

  BasicState basic_state = BasicState::kSitting;
  Gait gait = Gait::kWalk;
  ControlMode control_mode = ControlMode::kManual;
  int body_height_gear = 0;  // -1 = 匍匐, 0 = 正常

  // 世界系位姿（腿式里程计，会累积漂移，仅供参考，正式定位以 SLAM 为准）
  float odom_x = 0.0f;
  float odom_y = 0.0f;
  float odom_yaw = 0.0f;

  // 机体系速度
  float vel_x = 0.0f;
  float vel_y = 0.0f;
  float vel_yaw = 0.0f;

  // 姿态角，单位度
  float roll = 0.0f;
  float pitch = 0.0f;
  float yaw = 0.0f;

  float joint_pos[12] = {};
  float joint_vel[12] = {};
  float joint_tau[12] = {};

  float motor_temperature[12] = {};
  uint8_t driver_temperature[12] = {};
  float cpu_temperature = 0.0f;

  uint8_t battery_level = 0;
  float battery_voltage = 0.0f;

  int32_t current_mileage_cm = 0;
  uint32_t error_state = 0;
  uint8_t emergency_source = 0;
};

// 把 error_state 位域翻译成人可读的告警列表，无告警时返回空串。
std::string DescribeErrors(uint32_t error_state);

class MotionClient {
 public:
  explicit MotionClient(MotionClientConfig config);
  ~MotionClient();

  MotionClient(const MotionClient&) = delete;
  MotionClient& operator=(const MotionClient&) = delete;

  bool Start(std::string* error);
  void Stop();

  // --- 离散指令 -----------------------------------------------------------
  // 这些指令不会覆盖机器人自身的控制逻辑。比如趴着的时候发起步，运动程序会
  // 直接忽略。所以调用方应当依据 Snapshot().basic_state 判断是否该发，
  // 而不是假设发了就一定生效。

  void StandOrSit();        // 坐 <-> 站 切换
  void EnterTorqueStand();  // 初始站立 -> 力控站立
  void ToggleStepping();    // 力控站立 <-> 踏步 切换
  void SetGait(Gait gait);
  void SetBodyHeight(HeightGear gear);
  void SetControlMode(ControlMode mode);
  void SoftEmergencyStop();
  void SaveData(bool legacy_firmware = false);

  // --- 连续量 -------------------------------------------------------------
  // 需要持续调用来喂看门狗。停止调用后 command_timeout_ms 内轴指令自动归零。
  // 入参均为归一化的 [-1, 1]，由本类换算成 [-32767, 32767] 并处理死区与符号。

  // 仅在踏步状态下有效。vx 前正后负，vy 左正右负，wz 逆时针为正（右手系）。
  void SetVelocity(float vx, float vy, float wz);

  // 仅在力控站立状态下有效。四个量分别是抬升、横滚、俯仰、偏航。
  void SetPose(float height, float roll, float pitch, float yaw);

  // 主动放弃控制，立刻把轴指令清零。
  void ReleaseAxes();

  RobotState Snapshot() const;

 private:
  void TxLoop();
  void RxLoop();

  void SendSimple(uint32_t code, uint32_t value = 0);
  void SendAxis(uint32_t code, int32_t value);
  void HandleDatagram(const uint8_t* data, int len);

  static int32_t Normalize(float v);

  MotionClientConfig cfg_;
  UdpEndpoint tx_;  // 发指令，同时也是遥测的接收口
  std::thread tx_thread_;
  std::thread rx_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connect_confirmed_{false};

  // 轴指令的当前值与有效期。TX 线程按 axis_rate_hz 原样重发。
  mutable std::mutex axis_mutex_;
  int32_t axis_left_y_ = 0;
  int32_t axis_left_x_ = 0;
  int32_t axis_right_x_ = 0;
  int32_t axis_right_y_ = 0;
  std::chrono::steady_clock::time_point axis_deadline_{};

  mutable std::mutex state_mutex_;
  RobotState state_;
  std::chrono::steady_clock::time_point last_telemetry_{};
};

}  // namespace x30
