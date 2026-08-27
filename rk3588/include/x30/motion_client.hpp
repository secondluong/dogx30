// X30 运动控制客户端。
//
// 职责：
  //   - 仅在本端持有控制权时维持心跳并下发轴指令。没人 claim 时保持沉默，
  //     否则会和原厂 2.4G 手柄抢同一个 0x21 源：两边的心跳和全零轴互相覆盖，
  //     运动主机谁的都不听。交互式终端一启动就算持有。
  //   - 以 50 Hz 稳定下发轴指令（协议要求，超过 1 秒不发即失效）；
  //     只在力控站立和踏步态发，起立/坐下过渡期间停发，否则身高=0
  //     会把原厂柔和轨迹掐成猛起猛趴；
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
  bool battery_valid = false;

  int32_t current_mileage_cm = 0;
  uint32_t error_state = 0;
  uint8_t emergency_source = 0;

  // RL 起立之后运动主机仍报 basic_state=0。此标志表示我们已发起立、尚未发趴下。
  // 控制台用它显示「RL 站立」，避免芯片一直写「坐下」让人连点起立。
  bool rl_standing = false;
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

  void StandOrSit();        // 坐 <-> 站。发原厂手柄那对 RL 指令，不是文档里的旧切换
  void StandUp();           // 只起立，不看记忆
  void SitDown();           // 只趴下，不看记忆

  // 采纳遥控端告知的姿态，不向狗发任何指令。
  //
  // 为什么本机会不知道：2.4G 直连时起立/趴下由遥控器直接打给运动主机，不经过这里；
  // 而运动主机在 RL 起立后仍报 basic_state=0（见 protocol.hpp），遥测也认不出来。
  // 于是遥控端从 2.4G 切回 MESH 时，本机记的是「坐着」，按钮显示「起立」。
  // 这条就是让那份记忆对上。只有拿到控制权的客户端能调。
  // 记得站着并不等于能走：力控才调姿，起步才走。
  void AdoptPosture(bool standing);
  void UnloadForce();       // 卸力：急停后解除关节自锁，才能再起立
  void EnterTorqueStand();  // 进力控站立。若在踏步里先发一条踏步码退出。
  void StartStepping();     // 起步：已在踏步则不再发。否则先力控再延后一条踏步码。
  void StopStepping();      // 停步：待发就取消；已发出去再发一条退出。
  void ToggleStepping();    // 兼容旧客户端：按本端记忆翻转。

  void SetGait(Gait gait);
  // 站着点步态不要发给主机：文档写仅踏步态可切，RL 站着发会自己踏步。
  // 只记下；不要在推杆里冲出去 —— 杆一动就发爬坡，狗踏起来就停不掉。
  void QueueGait(Gait gait);
  void FlushQueuedGait();
  // 不是人按的起步：发力控把踏步停掉。遥测常把踏步报成坐下，不能只看 basic_state。
  void StopUnwantedMarch();
  bool UserStepping() const;
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

  // 是否向运动主机发心跳和轴。遥控服务在 claim 时打开、释放时关掉；
  // 关掉之后原厂 2.4G 手柄才能单独工作。
  void SetCommanding(bool on);
  bool commanding() const { return commanding_.load(); }

  RobotState Snapshot() const;

  // UDP 电池报文经常不到登记地址；感知主机的 /battery/* 作为后备。
  // from_udp 一旦成功，就不再让 ROS 覆盖。
  void ApplyBattery(uint8_t level, float voltage, bool from_udp);

  // RL 起立后运动 UDP 的腿式里程计/姿态/里程经常整段是 0。感知主机
  // /leg_odom、/mileage/*、IMU 仍在走，作为后备。UDP 一旦给出非零值就占住。
  void ApplyOdom(float x, float y, float yaw, float vx, float vy, float wz,
                 bool from_udp);
  void ApplyAtt(float roll, float pitch, float yaw, bool from_udp);
  void ApplyMileage(int32_t cm, bool from_udp);

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
  std::atomic<bool> commanding_{false};

  // 轴指令的当前值与有效期。TX 线程按 axis_rate_hz 原样重发。
  mutable std::mutex axis_mutex_;
  int32_t axis_left_y_ = 0;
  int32_t axis_left_x_ = 0;
  int32_t axis_right_x_ = 0;
  int32_t axis_right_y_ = 0;
  std::chrono::steady_clock::time_point axis_deadline_{};
  // 发过起立/趴下之后到这个时刻之前一律不发轴。遥测有几十毫秒滞后，那段时间里
  // basic_state 还报坐下，若此时已点力控，50 Hz 的身高=0
  // 正好打在起身轨迹的头上，把原厂柔和的起身掐成猛起。
  std::chrono::steady_clock::time_point axis_hold_until_{};
  // 只挡住连点起/趴，不挡力控之后立刻趴下。axis_hold 是给轴用的。
  std::chrono::steady_clock::time_point stand_sit_hold_until_{};

  mutable std::mutex state_mutex_;
  RobotState state_;
  std::chrono::steady_clock::time_point last_telemetry_{};
  bool battery_from_udp_{false};
  bool odom_from_udp_{false};
  bool odom_from_ros_{false};
  bool att_from_udp_{false};
  bool mileage_from_udp_{false};
  bool mileage_from_ros_{false};

  // 上次「坐/站」发出去的是起立还是趴下。RL 起立后遥测仍报坐下，
  // 只能靠这个决定下一次该发哪条，不能信 basic_state。
  enum class LastStandSit { kUnknown, kStood, kSat };
  LastStandSit last_stand_sit_{LastStandSit::kUnknown};

  // 操作员点了力控/起步。RL 起立后遥测仍报 0，单凭遥测会把轴吞掉；
  // 这条记下「可以发轴」。起立本身不置位。趴下/急停清掉。
  bool axes_unlocked_ = false;

  // 没遥测时记力控/踏步踩到哪了。踏步是切换指令，重发会停步。
  bool torqued_{false};
  bool stepping_{false};
  bool queued_gait_set_{false};
  Gait queued_gait_{Gait::kWalk};
  bool step_sent_{false};
  // 力控刚发就跟一条踏步，主机还在过渡里会丢掉。TxLoop 到点再发。
  std::chrono::steady_clock::time_point step_at_{};
  // 踏步里趴下会被主机丢掉。先停步，到点再发 RL 趴下。
  std::chrono::steady_clock::time_point sit_at_{};
};

}  // namespace x30
