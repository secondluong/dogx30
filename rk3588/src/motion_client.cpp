#include "x30/motion_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x30 {
namespace {

using Clock = std::chrono::steady_clock;

// 从原始报文里安全地取出一个结构体。长度不足就返回 false，避免越界读。
template <typename T>
bool Extract(const uint8_t* data, int len, T* out) {
  if (len < static_cast<int>(sizeof(CommandHead) + sizeof(T))) return false;
  std::memcpy(out, data + sizeof(CommandHead), sizeof(T));
  return true;
}

}  // namespace

std::string DescribeErrors(uint32_t error_state) {
  static constexpr const char* kNames[] = {
      "IMU 超时",   "心跳超时",   "驱动器过温", "驱动器故障",
      "电机过温",   "电量低",     "电池过热",   "GPIO 故障",
      "CPU 过温",   "CPU 降频",
  };
  std::string out;
  for (int i = 0; i < 10; ++i) {
    if (error_state & (1u << i)) {
      if (!out.empty()) out += ", ";
      out += kNames[i];
    }
  }
  return out;
}

MotionClient::MotionClient(MotionClientConfig config)
    : cfg_(std::move(config)) {}

MotionClient::~MotionClient() { Stop(); }

bool MotionClient::Start(std::string* error) {
  if (running_.load()) return true;

  if (!tx_.Open(cfg_.local_port, error)) return false;
  if (!tx_.SetPeer(cfg_.robot_ip, cfg_.robot_port, error)) return false;

  running_.store(true);
  connect_confirmed_.store(false);
  tx_thread_ = std::thread(&MotionClient::TxLoop, this);
  rx_thread_ = std::thread(&MotionClient::RxLoop, this);
  return true;
}

void MotionClient::Stop() {
  if (!running_.exchange(false)) return;
  if (tx_thread_.joinable()) tx_thread_.join();
  if (rx_thread_.joinable()) rx_thread_.join();
  tx_.Close();
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

void MotionClient::SendSimple(uint32_t code, uint32_t value) {
  CommandHead head{};
  head.code = code;
  head.paramters_size = value;
  head.type = kTypeSimple;
  tx_.Send(&head, sizeof(head));
}

void MotionClient::SendAxis(uint32_t code, int32_t value) {
  // 轴指令走简单指令通道，指令值即轴的原始读数，按位重解释成 uint32。
  uint32_t raw;
  std::memcpy(&raw, &value, sizeof(raw));
  SendSimple(code, raw);
}

void MotionClient::TxLoop() {
  const auto axis_period =
      std::chrono::microseconds(1000000 / std::max(1, cfg_.axis_rate_hz));
  const int heartbeat_every =
      std::max(1, cfg_.axis_rate_hz / std::max(1, cfg_.heartbeat_rate_hz));

  auto next = Clock::now();
  int tick = 0;

  while (running_.load()) {
    if (!commanding_.load()) {
      // 不发心跳、不发轴。原厂手柄走 2.4G 也是 0x21 源，我们一开机就抢，
      // 两边会把对方的指令盖成全零，运动主机表现为谁都控不了。
      connect_confirmed_.store(false);
      ++tick;
      next += axis_period;
      const auto now = Clock::now();
      if (next < now) next = now + axis_period;
      std::this_thread::sleep_until(next);
      continue;
    }

    // 心跳必须先于一切。丢心跳的后果比丢一帧轴指令严重得多。
    if (tick % heartbeat_every == 0) {
      SendSimple(cmd::kHeartbeat);
      // 协议要求在心跳开始后补发一次连接确认。
      if (!connect_confirmed_.exchange(true)) {
        SendSimple(cmd::kConnectConfirm);
      }
    }

    bool send_axes = true;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      // 遥测还没来或已经断时，不知道当前基础状态，沿用旧行为继续发轴——
      // 否则 network.toml 没登记、一条遥测都没有时，力控姿态也发不出去。
      if (state_.telemetry_alive) {
        send_axes = AxisCommandsApply(
            state_.basic_state,
            last_stand_sit_ == LastStandSit::kStood || axes_unlocked_);
      }
    }

    if (send_axes) {
      int32_t ly, lx, rx, ry;
      {
        std::lock_guard<std::mutex> lock(axis_mutex_);
        // 看门狗：上层不再喂数据就归零，机器人会自行减速到停。
        if (Clock::now() > axis_deadline_) {
          axis_left_y_ = axis_left_x_ = axis_right_x_ = axis_right_y_ = 0;
        }
        ly = axis_left_y_;
        lx = axis_left_x_;
        rx = axis_right_x_;
        ry = axis_right_y_;
      }

      SendAxis(cmd::kAxisLeftY, ly);
      SendAxis(cmd::kAxisLeftX, lx);
      SendAxis(cmd::kAxisRightX, rx);
      SendAxis(cmd::kAxisRightY, ry);
    }

    ++tick;
    next += axis_period;
    // 如果本轮超时太多就重新对齐，避免累积误差后疯狂追帧。
    const auto now = Clock::now();
    if (next < now) {
      next = now + axis_period;
    }
    std::this_thread::sleep_until(next);
  }
}

// ---------------------------------------------------------------------------
// 接收
// ---------------------------------------------------------------------------

void MotionClient::RxLoop() {
  uint8_t buffer[2048];
  while (running_.load()) {
    const int n = tx_.Recv(buffer, sizeof(buffer), 100);
    if (n > 0) {
      HandleDatagram(buffer, n);
    }

    // 遥测心跳检测与发送解耦：即便一直收不到，也不影响我们继续发心跳。
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto age = Clock::now() - last_telemetry_;
    state_.telemetry_alive =
        last_telemetry_.time_since_epoch().count() != 0 &&
        age < std::chrono::milliseconds(cfg_.telemetry_timeout_ms);
  }
}

void MotionClient::HandleDatagram(const uint8_t* data, int len) {
  if (len < static_cast<int>(sizeof(CommandHead))) return;

  CommandHead head{};
  std::memcpy(&head, data, sizeof(head));

  std::lock_guard<std::mutex> lock(state_mutex_);
  last_telemetry_ = Clock::now();

  switch (head.code) {
    case telem::kRunningStatus: {
      RcsData d{};
      if (!Extract(data, len, &d)) return;
      state_.current_mileage_cm = d.current_mileage;
      state_.error_state = d.error_state;
      state_.emergency_source = d.rcs_state_list.emergency_source;
      state_.control_mode = d.rcs_state_list.is_nav_mode == 0
                                ? ControlMode::kManual
                                : ControlMode::kNonManual;
      break;
    }
    case telem::kMotionStatus: {
      MotionStateData d{};
      if (!Extract(data, len, &d)) return;
      state_.basic_state = static_cast<BasicState>(d.basic_state);
      state_.gait = static_cast<Gait>(d.gait_state);
      state_.odom_x = d.leg_odom_pos[0];
      state_.odom_y = d.leg_odom_pos[1];
      state_.odom_yaw = d.leg_odom_pos[2];
      state_.vel_x = d.leg_odom_vel[0];
      state_.vel_y = d.leg_odom_vel[1];
      state_.vel_yaw = d.leg_odom_vel[2];
      break;
    }
    case telem::kSensorData: {
      ControllerSensorData d{};
      if (!Extract(data, len, &d)) return;
      state_.roll = d.imu_data.roll;
      state_.pitch = d.imu_data.pitch;
      state_.yaw = d.imu_data.yaw;
      std::memcpy(state_.joint_pos, d.joint_pos.data, sizeof(state_.joint_pos));
      std::memcpy(state_.joint_vel, d.joint_vel.data, sizeof(state_.joint_vel));
      std::memcpy(state_.joint_tau, d.joint_tau.data, sizeof(state_.joint_tau));
      break;
    }
    case telem::kSafeData: {
      ControllerSafeData d{};
      if (!Extract(data, len, &d)) return;
      std::memcpy(state_.motor_temperature, d.motor_temperature,
                  sizeof(state_.motor_temperature));
      std::memcpy(state_.driver_temperature, d.driver_temperture,
                  sizeof(state_.driver_temperature));
      state_.cpu_temperature = d.cpu_info_.temperature;
      break;
    }
    case telem::kBattery: {
      BatterySensorData d{};
      if (!Extract(data, len, &d)) return;
      state_.battery_level = d.battery_level;
      state_.battery_voltage = static_cast<float>(d.voltage);
      break;
    }
    case telem::kBodyHeightState: {
      // 简单报文，档位直接放在 paramters_size 里，按有符号解释。
      int32_t gear;
      std::memcpy(&gear, &head.paramters_size, sizeof(gear));
      state_.body_height_gear = gear;
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// 离散指令
// ---------------------------------------------------------------------------

void MotionClient::StandOrSit() {
  const RobotState s = Snapshot();
  // 过渡还没走完再发一次，轻则把柔和轨迹掐断，重则当场反转（坐到一半又站）。
  // RL 起立后 basic_state 仍是「坐下」，这条拦不住 RL 过渡，只拦文档里那套状态机。
  if (s.telemetry_alive && IsStandSitTransient(s.basic_state)) {
    std::printf("[运动] 忽略坐/站：当前正在%s\n", ToString(s.basic_state));
    return;
  }
  ReleaseAxes();

  // 遥测在 RL 站着时仍报 basic_state=0。现场 10:45 起了一次之后连点四次「坐」，
  // 运动主机收到的全是 0x21010223（再起立），一条趴下都没有。
  // 原厂手柄自己记得现在是站着，发 0x21010222。我们也得自己记。
  bool sitting;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const bool telem_upright =
        s.telemetry_alive &&
        (s.basic_state == BasicState::kInitialStanding ||
         s.basic_state == BasicState::kTorqueStanding ||
         s.basic_state == BasicState::kStepping);
    if (telem_upright) {
      sitting = false;
    } else if (s.telemetry_alive &&
               s.basic_state == BasicState::kEmergencyOrFall) {
      sitting = true;
      last_stand_sit_ = LastStandSit::kSat;
    } else if (last_stand_sit_ == LastStandSit::kStood) {
      sitting = false;
    } else {
      sitting = true;
    }
    last_stand_sit_ = sitting ? LastStandSit::kStood : LastStandSit::kSat;
    state_.rl_standing = (last_stand_sit_ == LastStandSit::kStood);
    if (!sitting) axes_unlocked_ = false;
  }

  if (sitting) {
    std::printf("[运动] RL 起立 0x21010223（遥测=%s）\n", ToString(s.basic_state));
    SendSimple(cmd::kRlStandUp);
  } else {
    std::printf("[运动] RL 趴下 0x21010222（遥测=%s）\n", ToString(s.basic_state));
    SendSimple(cmd::kRlSitDown);
  }
}
void MotionClient::EnterTorqueStand() {
  SendSimple(cmd::kTorqueStand);
  std::lock_guard<std::mutex> lock(state_mutex_);
  axes_unlocked_ = true;
}
void MotionClient::ToggleStepping() {
  SendSimple(cmd::kSteppingToggle);
  std::lock_guard<std::mutex> lock(state_mutex_);
  axes_unlocked_ = true;
}

void MotionClient::SetGait(Gait gait) {
  const uint32_t code = GaitCommandCode(gait);
  if (code != 0) SendSimple(code);
}

void MotionClient::SetBodyHeight(HeightGear gear) {
  SendSimple(cmd::kBodyHeight, static_cast<uint32_t>(gear));
}

void MotionClient::SetControlMode(ControlMode mode) {
  SendSimple(mode == ControlMode::kManual ? cmd::kModeManual
                                          : cmd::kModeNonManual);
}

void MotionClient::SoftEmergencyStop() {
  // 急停要立刻走线，不能排在下一帧轴指令后面。同时把轴清零，
  // 免得急停解除后残留的速度指令又把机器人推出去。
  SendSimple(cmd::kSoftEmergencyStop);
  ReleaseAxes();
  std::lock_guard<std::mutex> lock(state_mutex_);
  last_stand_sit_ = LastStandSit::kSat;
  state_.rl_standing = false;
  axes_unlocked_ = false;
}

void MotionClient::SaveData(bool legacy_firmware) {
  SendSimple(legacy_firmware ? cmd::kSaveDataLegacy : cmd::kSaveData);
}

// ---------------------------------------------------------------------------
// 连续量
// ---------------------------------------------------------------------------

int32_t MotionClient::Normalize(float v) {
  if (!std::isfinite(v)) return 0;
  v = std::max(-1.0f, std::min(1.0f, v));
  auto raw = static_cast<int32_t>(v * static_cast<float>(kAxisMax));
  // 死区内的值机器人本来就当零处理，这里提前抹平，让回传的 joystick 值干净些。
  if (std::abs(raw) < kAxisDeadZone) return 0;
  return std::max(-kAxisMax, std::min(kAxisMax, raw));
}

void MotionClient::SetVelocity(float vx, float vy, float wz) {
  std::lock_guard<std::mutex> lock(axis_mutex_);
  // 协议里 Y 向线速度与偏航角速度的映射带一个负号：轴为正表示向右平移 /
  // 向右转，而机体系 Y 轴左为正、偏航逆时针为正，故此处取反。
  axis_left_y_ = Normalize(vx);
  axis_left_x_ = Normalize(-vy);
  axis_right_x_ = Normalize(-wz);
  axis_right_y_ = 0;  // 踏步态下右摇杆 Y 无定义
  axis_deadline_ = Clock::now() + std::chrono::milliseconds(cfg_.command_timeout_ms);
}

void MotionClient::SetPose(float height, float roll, float pitch, float yaw) {
  std::lock_guard<std::mutex> lock(axis_mutex_);
  axis_left_y_ = Normalize(height);
  axis_left_x_ = Normalize(roll);
  axis_right_x_ = Normalize(yaw);
  axis_right_y_ = Normalize(pitch);
  axis_deadline_ = Clock::now() + std::chrono::milliseconds(cfg_.command_timeout_ms);
}

void MotionClient::ReleaseAxes() {
  std::lock_guard<std::mutex> lock(axis_mutex_);
  axis_left_y_ = axis_left_x_ = axis_right_x_ = axis_right_y_ = 0;
  axis_deadline_ = Clock::time_point{};
}

void MotionClient::SetCommanding(bool on) {
  const bool was = commanding_.exchange(on);
  if (on && !was) {
    connect_confirmed_.store(false);
    std::printf("[运动] 本端接管：开始向运动主机发心跳\n");
  } else if (!on && was) {
    std::printf("[运动] 本端松开：停止向运动主机发心跳，原厂手柄可单独接管\n");
  }
}

RobotState MotionClient::Snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  RobotState s = state_;
  s.rl_standing = (last_stand_sit_ == LastStandSit::kStood);
  return s;
}

}  // namespace x30
