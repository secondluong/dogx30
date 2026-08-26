#include "x30/motion_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x30 {
namespace {

using Clock = std::chrono::steady_clock;

// 起立/趴下之后停发轴多久。够盖住遥测把过渡态报上来的滞后（实测几十毫秒），
// 又远短于起身本身的两秒，所以不影响「站起来就能推杆走」。
constexpr int kAxisHoldMs = 300;
// 力控和踏步之间要留时间：主机还在过渡里，紧跟着的踏步会被丢掉。
constexpr int kArmGapMs = 500;

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
    {
      // 起步在没力控时会先发力控，到点再补踏步。必须在 TX 线程发，别另开定时器。
      bool due = false;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (step_at_ != Clock::time_point{} && Clock::now() >= step_at_) {
          step_at_ = {};
          stepping_ = true;
          due = true;
        }
      }
      if (due) SendSimple(cmd::kSteppingToggle);
    }

    if (tick % heartbeat_every == 0) {
      SendSimple(cmd::kHeartbeat);
      // 协议要求在心跳开始后补发一次连接确认。
      if (!connect_confirmed_.exchange(true)) {
        SendSimple(cmd::kConnectConfirm);
      }
    }

    bool send_axes = true;
    {
      // 起立/趴下刚发出去，遥测还没报出过渡态的这几十毫秒里也不发轴 ——
      // 光靠遥测判断会漏掉这一段，实测能漏出四五帧身高=0。
      std::lock_guard<std::mutex> lock(axis_mutex_);
      if (Clock::now() < axis_hold_until_) send_axes = false;
    }
    if (send_axes) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      // 遥测还没来或已经断时，不知道当前基础状态，沿用旧行为继续发轴——
      // 否则 network.toml 没登记、一条遥测都没有时，力控姿态也发不出去。
      if (state_.telemetry_alive) {
        send_axes = AxisCommandsApply(
            state_.basic_state,
            axes_unlocked_ || torqued_ || stepping_);
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

void MotionClient::ApplyBattery(uint8_t level, float voltage, bool from_udp) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!from_udp && battery_from_udp_) return;
  if (from_udp) battery_from_udp_ = true;
  if (level > 100) level = 100;
  state_.battery_level = level;
  if (voltage > 0.0f) state_.battery_voltage = voltage;
  state_.battery_valid = state_.battery_level > 0 || state_.battery_voltage > 1.0f;
}

void MotionClient::ApplyOdom(float x, float y, float yaw, float vx, float vy,
                             float wz, bool from_udp) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const bool live = std::hypot(x, y) > 0.05f || std::hypot(vx, vy) > 0.02f;
  if (from_udp) {
    if (live) odom_from_udp_ = true;
    else if (!odom_from_udp_ && odom_from_ros_) return;
  } else {
    if (odom_from_udp_) return;
    odom_from_ros_ = true;
  }
  state_.odom_x = x;
  state_.odom_y = y;
  state_.odom_yaw = yaw;
  state_.vel_x = vx;
  state_.vel_y = vy;
  state_.vel_yaw = wz;
}

void MotionClient::ApplyAtt(float roll, float pitch, float yaw, bool from_udp) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const bool live =
      std::fabs(roll) + std::fabs(pitch) + std::fabs(yaw) > 0.5f;
  if (from_udp) {
    if (live) att_from_udp_ = true;
    else if (!att_from_udp_) return;
  } else if (att_from_udp_) {
    return;
  }
  state_.roll = roll;
  state_.pitch = pitch;
  state_.yaw = yaw;
}

void MotionClient::ApplyMileage(int32_t cm, bool from_udp) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (from_udp) {
    if (cm > 0) mileage_from_udp_ = true;
    else if (!mileage_from_udp_ && mileage_from_ros_) return;
  } else {
    if (mileage_from_udp_) return;
    mileage_from_ros_ = true;
  }
  state_.current_mileage_cm = cm;
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
      if (d.current_mileage > 0) {
        mileage_from_udp_ = true;
        state_.current_mileage_cm = d.current_mileage;
      } else if (!mileage_from_ros_) {
        state_.current_mileage_cm = d.current_mileage;
      }
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
      {
        const float ox = d.leg_odom_pos[0];
        const float oy = d.leg_odom_pos[1];
        const bool live =
            std::hypot(ox, oy) > 0.05f ||
            std::hypot(d.leg_odom_vel[0], d.leg_odom_vel[1]) > 0.02f;
        if (live) {
          odom_from_udp_ = true;
        }
        if (live || !odom_from_ros_) {
          state_.odom_x = ox;
          state_.odom_y = oy;
          state_.odom_yaw = d.leg_odom_pos[2];
          state_.vel_x = d.leg_odom_vel[0];
          state_.vel_y = d.leg_odom_vel[1];
          state_.vel_yaw = d.leg_odom_vel[2];
        }
      }
      break;
    }
    case telem::kSensorData: {
      ControllerSensorData d{};
      if (!Extract(data, len, &d)) return;
      if (std::fabs(d.imu_data.roll) + std::fabs(d.imu_data.pitch) +
          std::fabs(d.imu_data.yaw) > 0.5f) {
        att_from_udp_ = true;
      }
      if (att_from_udp_) {
        state_.roll = d.imu_data.roll;
        state_.pitch = d.imu_data.pitch;
        state_.yaw = d.imu_data.yaw;
      }
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
      // 实机报文经常短于文档的 40 字节；整包对不上就整段丢掉，电量会永远是 0。
      BatterySensorData d{};
      const int payload = len - static_cast<int>(sizeof(CommandHead));
      if (payload > 0) {
        std::memcpy(&d, data + sizeof(CommandHead),
                    std::min(payload, static_cast<int>(sizeof(d))));
      }
      float volts = static_cast<float>(d.voltage);
      if (d.voltage > 200) volts = static_cast<float>(d.voltage) / 100.0f;
      if (d.voltage == 0 && payload >= 4) {
        float fv = 0.0f;
        std::memcpy(&fv, data + sizeof(CommandHead), sizeof(fv));
        if (fv > 20.0f && fv < 100.0f) volts = fv;
      }
      uint8_t level = d.battery_level;
      if (level > 100) level = 0;
      static bool logged = false;
      if (!logged) {
        logged = true;
        std::printf("[运动] 电池 UDP len=%d level=%u voltage=%.1f (raw=%u)\n",
                    len, static_cast<unsigned>(level), volts,
                    static_cast<unsigned>(d.voltage));
      }
      battery_from_udp_ = true;
      if (level > 100) level = 100;
      state_.battery_level = level;
      if (volts > 0.0f) state_.battery_voltage = volts;
      state_.battery_valid = state_.battery_level > 0 ||
                             state_.battery_voltage > 1.0f;
      break;
    }
    case telem::kBodyHeightState: {
      // 简单报文，档位直接放在 paramters_size 里，按有符号解释。
      int32_t gear;
      std::memcpy(&gear, &head.paramters_size, sizeof(gear));
      state_.body_height_gear = gear;
      break;
    }
    default: {
      static uint32_t seen[16] = {};
      static int nseen = 0;
      bool already = false;
      for (int i = 0; i < nseen; ++i) {
        if (seen[i] == head.code) already = true;
      }
      if (!already && nseen < 16) {
        seen[nseen++] = head.code;
        std::printf("[运动] 未识别遥测 0x%08x len=%d\n", head.code, len);
      }
      break;
    }
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
    if (!sitting) {
      axes_unlocked_ = false;
      torqued_ = false;
      stepping_ = false;
      step_at_ = {};
    }
  }

  if (sitting) {
    std::printf("[运动] RL 起立 0x21010223（遥测=%s）\n", ToString(s.basic_state));
    SendSimple(cmd::kRlStandUp);
  } else {
    std::printf("[运动] RL 趴下 0x21010222（遥测=%s）\n", ToString(s.basic_state));
    SendSimple(cmd::kRlSitDown);
  }
}

void MotionClient::StandUp() {
  {
    std::lock_guard<std::mutex> lock(axis_mutex_);
    if (Clock::now() < axis_hold_until_) {
      std::printf("[运动] 忽略起立：刚发过起/趴\n");
      return;
    }
  }
  ReleaseAxes();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_stand_sit_ = LastStandSit::kStood;
    state_.rl_standing = true;
    axes_unlocked_ = false;
    torqued_ = false;
    stepping_ = false;
    step_at_ = {};
  }
  std::printf("[运动] RL 起立 0x21010223（遥测=%s）\n",
              ToString(Snapshot().basic_state));
  SendSimple(cmd::kRlStandUp);
}

void MotionClient::SitDown() {
  {
    std::lock_guard<std::mutex> lock(axis_mutex_);
    if (Clock::now() < axis_hold_until_) {
      std::printf("[运动] 忽略趴下：刚发过起/趴\n");
      return;
    }
  }
  ReleaseAxes();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_stand_sit_ = LastStandSit::kSat;
    state_.rl_standing = false;
    axes_unlocked_ = false;
    torqued_ = false;
    stepping_ = false;
    step_at_ = {};
  }
  std::printf("[运动] RL 趴下 0x21010222（遥测=%s）\n",
              ToString(Snapshot().basic_state));
  SendSimple(cmd::kRlSitDown);
}

void MotionClient::AdoptPosture(bool standing) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  last_stand_sit_ = standing ? LastStandSit::kStood : LastStandSit::kSat;
  state_.rl_standing = standing;
  // 切档只交接站没站。力控/起步是切换指令，带着上一条链路的记忆会发反。
  axes_unlocked_ = false;
  torqued_ = false;
  stepping_ = false;
  step_at_ = {};
  std::printf("[运动] 采纳遥控端告知的姿态：%s（遥测=%s）\n",
              standing ? "站立" : "坐下", ToString(state_.basic_state));
}

void MotionClient::UnloadForce() {
  // 软急停后关节自锁。原厂 App「卸力」、手柄 ⑤/㉑ 打的是 0x21010202，
  // 不是 RL 起/趴。现场急停后再发 0x21010223/22，主机不应，狗起不来。
  SendSimple(cmd::kUnloadForce);
  ReleaseAxes();
  std::lock_guard<std::mutex> lock(state_mutex_);
  last_stand_sit_ = LastStandSit::kSat;
  state_.rl_standing = false;
  axes_unlocked_ = false;
  torqued_ = false;
  stepping_ = false;
  step_at_ = {};
  std::printf("[运动] 卸力 0x21010202（遥测=%s）\n", ToString(state_.basic_state));
}

void MotionClient::EnterTorqueStand() {
  const RobotState s = Snapshot();
  bool stop_step = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    step_at_ = {};
    // 踏步指令是切换。本地 stepping_ 切档后会撒谎，只能信主机报的踏步。
    stop_step = s.telemetry_alive && s.basic_state == BasicState::kStepping;
    stepping_ = false;
    torqued_ = true;
    axes_unlocked_ = true;
  }
  // 已经在踏步时再发力控，主机会忽略，狗继续原地踏。先切一次踏步停下来。
  if (stop_step) SendSimple(cmd::kSteppingToggle);
  else SendSimple(cmd::kTorqueStand);
}

void MotionClient::ToggleStepping() {
  SendSimple(cmd::kSteppingToggle);
  std::lock_guard<std::mutex> lock(state_mutex_);
  stepping_ = !stepping_;
  axes_unlocked_ = true;
}

void MotionClient::EnterStepping() {
  const RobotState s = Snapshot();
  bool already = false;
  bool need_torque = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    already = s.telemetry_alive && s.basic_state == BasicState::kStepping;
    if (already) {
      stepping_ = true;
      axes_unlocked_ = true;
      return;
    }
    if (step_at_ != Clock::time_point{}) {
      axes_unlocked_ = true;
      return;
    }
    const bool in_torque =
        s.telemetry_alive && s.basic_state == BasicState::kTorqueStanding;
    need_torque = !in_torque;
    torqued_ = true;
    axes_unlocked_ = true;
    if (need_torque) {
      // 先力控，到点再踏步。立刻跟一条会被丢掉，表现为点了起步却只在原地力控。
      step_at_ = Clock::now() + std::chrono::milliseconds(kArmGapMs);
    } else {
      stepping_ = true;
      step_at_ = {};
    }
  }
  if (need_torque) SendSimple(cmd::kTorqueStand);
  else SendSimple(cmd::kSteppingToggle);
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
  torqued_ = false;
  stepping_ = false;
  step_at_ = {};
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

// 曾经在这里替操作员把「力控站立」「踏步」这两级台阶踩掉（推杆就走）。撤了：
// 踏步是**切换**指令，程序补的那一条会和操作员自己按的那一条抵消，狗刚起步又停
// 下，而人完全看不出是谁发的。现在仍然要人按这两下，漏了由 App 出声提醒
// （web/app.js 的 checkNeedArm）。
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
  axis_hold_until_ = Clock::now() + std::chrono::milliseconds(kAxisHoldMs);
}

void MotionClient::SetCommanding(bool on) {
  const bool was = commanding_.exchange(on);
  if (on && !was) {
    connect_confirmed_.store(false);
    std::printf("[运动] 本端接管：开始向运动主机发心跳\n");
  } else if (!on && was) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    axes_unlocked_ = false;
    torqued_ = false;
    stepping_ = false;
    step_at_ = {};
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
