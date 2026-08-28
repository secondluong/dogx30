#include "x30/motion_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace x30 {
namespace {

using Clock = std::chrono::steady_clock;

// 起立/趴下之后停发轴多久。够盖住遥测把过渡态报上来的滞后（实测几十毫秒），
// 又远短于起身本身的两秒，不挡后面的力控/起步。
constexpr int kAxisHoldMs = 300;
constexpr int kSitAfterStepMs = 300;
// /robot_basic_state 是低频状态话题，真机上的相邻报文可能超过 1.2 秒。
// 过短会让状态在 ROS 站立与 UDP 错报趴下之间来回切换，导致按钮和摇杆闪烁。
constexpr int kRosStateFreshMs = 5000;

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
      // 起立/趴下刚发出去，遥测还没报出过渡态的这几十毫秒里也不发轴 ——
      // 光靠遥测判断会漏掉这一段，实测能漏出四五帧身高=0。
      std::lock_guard<std::mutex> lock(axis_mutex_);
      if (Clock::now() < axis_hold_until_) send_axes = false;
    }
    if (send_axes) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      const bool ros_upright =
          ros_basic_at_ != Clock::time_point{} &&
          Clock::now() - ros_basic_at_ < std::chrono::milliseconds(kRosStateFreshMs) &&
          (state_.ros_basic_state == 2 || state_.ros_basic_state == 3 ||
           state_.ros_basic_state == 4 || state_.ros_basic_state == 16);
      const bool safe_upright =
          state_.telemetry_alive &&
          !JointsLocked(state_.basic_state, state_.emergency_source) &&
          !IsStandSitTransient(state_.basic_state) &&
          (TelemUpright(state_.basic_state) || state_.rl_standing ||
           ros_upright);
      // 力控只发姿态，起步只发速度。主机在初始站立里把同一组轴当速度，
      // 力控左杆就会走路、俯仰轴被丢掉。RL 主机又可能在已站立后继续报
      // basic_state=0，所以不能要求它必须报 3/4；控制层已发出的模式指令才
      // 决定轴含义，主机遥测只负责失联、急停、起趴过渡和姿态安全门。
      // TCP 监控只有低频查询响应，绝不能单独放开 50Hz 轴。它只作为额外
      // 锁定条件；真正的轴安全门始终要求运动 UDP 新鲜。
      const bool body_locked =
          state_.body_monitor_alive &&
          (state_.body_motion_state == 6 || state_.body_motion_state == 7);
      const bool safe_state = safe_upright && !body_locked;
      if (stepping_ && step_sent_) {
        send_axes = safe_state;
      } else if (torqued_ && !stepping_) {
        const bool torque_confirmed =
            state_.basic_state == BasicState::kTorqueStanding ||
            state_.ros_basic_state == 3 || state_.body_motion_state == 3;
        const bool fallback_ready =
            pose_axes_at_ != Clock::time_point{} && Clock::now() >= pose_axes_at_;
        send_axes = safe_state && (torque_confirmed || fallback_ready);
      } else {
        send_axes = false;
      }
    }

    bool fire_step = false;
    bool step_feedback_timeout = false;
    bool fire_sit = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (step_at_ != Clock::time_point{} && Clock::now() >= step_at_) {
        const bool torque_confirmed =
            state_.body_motion_state == 3 || state_.ros_basic_state == 3 ||
            state_.basic_state == BasicState::kTorqueStanding;
        if (state_.body_monitor_alive && !torque_confirmed) {
          if (Clock::now() - motion_command_at_ < std::chrono::milliseconds(2500)) {
            step_at_ = Clock::now() + std::chrono::milliseconds(100);
          } else {
            step_at_ = {};
            stepping_ = false;
            torqued_ = false;
            motion_phase_ = MotionPhase::kStopped;
            step_feedback_timeout = true;
          }
        } else {
          step_at_ = {};
          step_sent_ = true;
          motion_phase_ = MotionPhase::kWalking;
          fire_step = true;
        }
      }
      if (sit_at_ != Clock::time_point{} && Clock::now() >= sit_at_) {
        sit_at_ = {};
        fire_sit = true;
      }
    }
    if (fire_step) {
      SendSimple(cmd::kSteppingToggle);
      std::printf("[运动] 踏步切换 → 起步 0x21010201（力控之后）\n");
    }
    if (step_feedback_timeout) {
      std::printf("[运动] 起步取消：本体监控未确认进入力控状态 3\n");
    }
    if (fire_sit) {
      SendSimple(cmd::kStandSitToggle);
      std::printf("[运动] 停步之后官方起趴切换 0x21010202\n");
    }

    // 用户可以在力控/停步时先选配置，但只有狗主机确认进入踏步态后才执行。
    // 不能紧跟踏步切换码发送：主机还在过渡时会静默丢掉步态。
    bool flush_profile = false;
    bool flush_height = false;
    HeightGear queued_height = HeightGear::kNormal;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      // 起步码已经发出就执行预选配置。现场 basic_state 和 gait 都可能长期保留
      // 旧值，继续等“确认”会让步态/身高永远卡在队列中。
      flush_profile = step_sent_;
      if (flush_profile && queued_height_set_) {
        flush_height = true;
        queued_height = queued_height_;
        queued_height_set_ = false;
        height_waits_for_gait_ = false;
      }
    }
    if (flush_profile) {
      // 先切步态再切身高；匍匐档下主机会拒绝越野等步态码。
      FlushQueuedGait();
      if (flush_height) {
        SendSimple(cmd::kBodyHeight, static_cast<uint32_t>(queued_height));
        std::lock_guard<std::mutex> lock(state_mutex_);
        height_cmd_ = queued_height;
        state_.body_height_gear =
            queued_height == HeightGear::kCrawl ? -1 : 0;
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

void MotionClient::ReconcileReportedMotionLocked(int motion_state) {
  if (motion_state != 3 && motion_state != 4) return;
  const auto now = Clock::now();
  if (motion_command_at_ != Clock::time_point{} &&
      now - motion_command_at_ < std::chrono::milliseconds(800)) {
    return;
  }
  if (motion_state == 4) {
    if (motion_phase_ == MotionPhase::kStopping) return;
    torqued_ = true;
    stepping_ = true;
    step_sent_ = true;
    step_at_ = {};
    motion_phase_ = MotionPhase::kWalking;
    return;
  }
  // 本体明确回到力控站立，纠正步态切换后仍残留的本地 walking 记忆。
  if (motion_phase_ == MotionPhase::kStarting) return;
  torqued_ = true;
  stepping_ = false;
  step_sent_ = false;
  step_at_ = {};
  motion_phase_ = MotionPhase::kStopped;
}

void MotionClient::ApplyBodyMonitor(bool alive, int motion_state,
                                    int gait_state, int motor_state,
                                    int charge_state, int control_mode,
                                    int location_state, int on_dock_state) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_.body_monitor_alive = alive;
  if (!alive) return;
  state_.body_motion_state = motion_state;
  state_.body_gait_state = gait_state;
  state_.body_motor_state = motor_state;
  state_.body_charge_state = charge_state;
  state_.body_control_mode = control_mode;
  state_.body_location_state = location_state;
  state_.body_on_dock_state = on_dock_state;
  if (motion_state == 6 || motion_state == 7) {
    axes_unlocked_ = false;
    torqued_ = false;
    stepping_ = false;
    step_sent_ = false;
    step_at_ = {};
    motion_phase_ = MotionPhase::kUnavailable;
  } else if (motion_state == 2 &&
             motion_phase_ == MotionPhase::kStopping) {
    motion_phase_ = MotionPhase::kStopped;
  } else {
    // Type=1002 是官方本体状态；不能因为有一条会谎报 0 的 UDP 就忽略它。
    ReconcileReportedMotionLocked(motion_state);
  }
}

void MotionClient::ApplyRosBasicState(int32_t state) {
  if (state < 0 || (state > 6 && state != 16)) return;
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_.ros_basic_state = state;
  state_.ros_motion_alive = true;
  ros_basic_at_ = Clock::now();
  ReconcileReportedMotionLocked(state);
}

void MotionClient::ApplyRosGaitState(int32_t gait) {
  if (gait < 0 || gait > 255) return;
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_.ros_gait_state = gait;
  ros_gait_at_ = Clock::now();
  if (!state_.telemetry_alive) state_.gait = static_cast<Gait>(gait);
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
      std::copy(std::begin(d.joystick), std::end(d.joystick),
                std::begin(state_.joystick));
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
      ReconcileReportedMotionLocked(d.basic_state);
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
      // 指令 0=匍匐 2=正常；遥测文档 −1=匍匐 0=正常。0 有歧义：刚发过匍匐
      // 就不要用 0 把控制台打钩顶回「正常」。
      int32_t gear;
      std::memcpy(&gear, &head.paramters_size, sizeof(gear));
      if (gear == 0 && height_cmd_ == HeightGear::kCrawl) {
        break;
      }
      if (gear < 0) {
        state_.body_height_gear = -1;
      } else if (gear == 2) {
        state_.body_height_gear = 0;
      } else {
        state_.body_height_gear = 0;
      }
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
  if (s.telemetry_alive && JointsLocked(s.basic_state, s.emergency_source)) {
    std::printf("[运动] 急停锁定中，坐/站改为卸力 0x21010202（遥测=%s 源=%u）\n",
                ToString(s.basic_state),
                static_cast<unsigned>(s.emergency_source));
    UnloadForce();
    return;
  }
  ReleaseAxes();

  // 旧平板会把 G20 起立/趴下都发成 name=stand（一条翻转）。上一轮若记下
  // 「站着」，遥测又报坐下，下一次起立就会走趴下 —— 现场「起立变成趴下」。
  // 遥测说趴着就起立；只有刚起过、遥测还在撒谎的那几秒，再按一次才是趴下。
  bool sitting;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const bool telem_upright =
        s.telemetry_alive && TelemUpright(s.basic_state);
    const auto now = Clock::now();
    const bool just_stood =
        last_stand_sit_ == LastStandSit::kStood &&
        last_stand_cmd_at_ != Clock::time_point{} &&
        now - last_stand_cmd_at_ < std::chrono::seconds(8);
    if (telem_upright) {
      sitting = false;
    } else if (s.telemetry_alive &&
               s.basic_state == BasicState::kEmergencyOrFall) {
      sitting = true;
      last_stand_sit_ = LastStandSit::kSat;
    } else if (just_stood) {
      sitting = false;
    } else {
      sitting = true;
    }
    last_stand_sit_ = sitting ? LastStandSit::kStood : LastStandSit::kSat;
    last_stand_cmd_at_ = now;
    state_.rl_standing = (last_stand_sit_ == LastStandSit::kStood);
    if (sitting) {
      axes_unlocked_ = false;
      torqued_ = false;
      stepping_ = false;
      step_sent_ = false;
      step_at_ = {};
      sit_at_ = {};
    }
  }

  if (sitting) {
    std::printf("[运动] 官方起立切换 0x21010202（遥测=%s）\n",
                ToString(s.basic_state));
    SendSimple(cmd::kStandSitToggle);
  } else {
    SitDown();
  }
}

void MotionClient::StandUp() {
  {
    const RobotState s = Snapshot();
    if (s.telemetry_alive &&
        JointsLocked(s.basic_state, s.emergency_source)) {
      std::printf("[运动] 急停锁定中，起立改为卸力 0x21010202（遥测=%s 源=%u）\n",
                  ToString(s.basic_state),
                  static_cast<unsigned>(s.emergency_source));
      UnloadForce();
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(axis_mutex_);
    if (Clock::now() < stand_sit_hold_until_ &&
        last_stand_sit_ == LastStandSit::kStood) {
      std::printf("[运动] 忽略起立：刚发过起立\n");
      return;
    }
    stand_sit_hold_until_ = Clock::now() + std::chrono::milliseconds(kAxisHoldMs);
  }
  ReleaseAxes();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_stand_sit_ = LastStandSit::kStood;
    last_stand_cmd_at_ = Clock::now();
    state_.rl_standing = true;
    axes_unlocked_ = false;
    torqued_ = false;
    stepping_ = false;
    motion_phase_ = MotionPhase::kStopped;
    queued_gait_set_ = false;
    queued_height_set_ = false;
    height_waits_for_gait_ = false;
    step_sent_ = false;
    step_at_ = {};
    sit_at_ = {};
    pose_axes_at_ = {};
  }
  std::printf("[运动] 官方起立切换 0x21010202（遥测=%s）\n",
              ToString(Snapshot().basic_state));
  SendSimple(cmd::kStandSitToggle);
}

void MotionClient::SitDown() {
  {
    const RobotState s = Snapshot();
    if (s.telemetry_alive &&
        JointsLocked(s.basic_state, s.emergency_source)) {
      std::printf("[运动] 急停锁定中，趴下改为卸力 0x21010202（遥测=%s 源=%u）\n",
                  ToString(s.basic_state),
                  static_cast<unsigned>(s.emergency_source));
      UnloadForce();
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(axis_mutex_);
    if (Clock::now() < stand_sit_hold_until_ &&
        last_stand_sit_ == LastStandSit::kSat) {
      std::printf("[运动] 忽略趴下：刚发过趴下\n");
      return;
    }
    stand_sit_hold_until_ = Clock::now() + std::chrono::milliseconds(kAxisHoldMs);
  }
  bool leave_step = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    leave_step = stepping_ && step_sent_;
  }
  if (leave_step) {
    SendSimple(cmd::kSteppingToggle);
    std::printf("[运动] 趴下前先停步 0x21010201\n");
  }
  ReleaseAxes();
  {
    const RobotState s = Snapshot();
    if (s.control_mode == ControlMode::kNonManual) {
      SetControlMode(ControlMode::kManual);
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_stand_sit_ = LastStandSit::kSat;
    last_stand_cmd_at_ = {};
    state_.rl_standing = false;
    axes_unlocked_ = false;
    torqued_ = false;
    stepping_ = false;
    motion_phase_ = MotionPhase::kUnavailable;
    queued_gait_set_ = false;
    queued_height_set_ = false;
    height_waits_for_gait_ = false;
    step_sent_ = false;
    step_at_ = {};
    if (leave_step) {
      sit_at_ = Clock::now() + std::chrono::milliseconds(kSitAfterStepMs);
    } else {
      sit_at_ = {};
    }
    pose_axes_at_ = {};
  }
  if (leave_step) {
    std::printf("[运动] 停步后再发 RL 趴下 0x21010222（遥测=%s）\n",
                ToString(Snapshot().basic_state));
    return;
  }
  std::printf("[运动] 官方趴下切换 0x21010202（遥测=%s）\n",
              ToString(Snapshot().basic_state));
  SendSimple(cmd::kStandSitToggle);
}

void MotionClient::AdoptPosture(bool standing) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  last_stand_sit_ = standing ? LastStandSit::kStood : LastStandSit::kSat;
  last_stand_cmd_at_ = standing ? Clock::now() : Clock::time_point{};
  state_.rl_standing = standing;
  // 切档只交接站没站。力控/起步是切换指令，带着上一条链路的记忆会发反。
  axes_unlocked_ = false;
  torqued_ = false;
  stepping_ = false;
  motion_phase_ = standing ? MotionPhase::kStopped : MotionPhase::kUnavailable;
  queued_gait_set_ = false;
  queued_height_set_ = false;
  height_waits_for_gait_ = false;
  step_sent_ = false;
  step_at_ = {};
  sit_at_ = {};
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
  last_stand_cmd_at_ = {};
  state_.rl_standing = false;
  state_.emergency_source = 0;
  axes_unlocked_ = false;
  torqued_ = false;
  stepping_ = false;
  motion_phase_ = MotionPhase::kUnavailable;
  queued_gait_set_ = false;
  queued_height_set_ = false;
  height_waits_for_gait_ = false;
  step_sent_ = false;
  step_at_ = {};
  sit_at_ = {};
  std::printf("[运动] 卸力 0x21010202（遥测=%s）\n", ToString(state_.basic_state));
}

void MotionClient::EnterTorqueStand() {
  const MotionView view = View();
  if (std::strcmp(view.posture, "standing") != 0) {
    std::printf("[运动] 忽略力控：当前姿态为 %s\n", view.posture);
    return;
  }
  bool leave_step = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    leave_step = stepping_ && step_sent_;
    step_at_ = {};
    step_sent_ = false;
    stepping_ = false;
    torqued_ = true;
    motion_phase_ = MotionPhase::kTorque;
    motion_command_at_ = Clock::now();
    axes_unlocked_ = true;
    sit_at_ = {};
    pose_axes_at_ = Clock::now() + std::chrono::milliseconds(1200);
  }
  // 还在踏步里俯仰轴无定义，速度轴也还在。先停步，再力控，轴停发一小会，
  // 免得 50 Hz 的「身高」在过渡里被主机读成前进。
  if (leave_step) SendSimple(cmd::kSteppingToggle);
  ReleaseAxes();
  SendSimple(cmd::kTorqueStand);
  std::printf("[运动] 力控站立 0x2101020A%s\n",
              leave_step ? "（先停步）" : "");
}

void MotionClient::StartStepping() {
  const MotionView view = View();
  if (view.phase != MotionPhase::kStopped &&
      view.phase != MotionPhase::kTorque) {
    std::printf("[运动] 忽略起步：当前规范状态为 %s\n", view.motion);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (stepping_ && (step_sent_ || step_at_ != Clock::time_point{})) {
      std::printf("[运动] 起步：已经在踏步，不重发\n");
      return;
    }
    stepping_ = true;
    torqued_ = true;
    axes_unlocked_ = true;
    step_sent_ = false;
    motion_phase_ = MotionPhase::kStarting;
    motion_command_at_ = Clock::now();
    step_at_ = Clock::now() + std::chrono::milliseconds(500);
  }
  SendSimple(cmd::kTorqueStand);
  std::printf("[运动] 起步：先力控 0x2101020A，稍后踏步\n");
}

void MotionClient::StopStepping() {
  bool send_step = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const bool pending = step_at_ != Clock::time_point{} && !step_sent_;
    send_step = (step_sent_ || state_.basic_state == BasicState::kStepping ||
                 (state_.body_monitor_alive &&
                  state_.body_motion_state == 4)) &&
                !pending;
    stepping_ = false;
    torqued_ = true;
    motion_phase_ = send_step ? MotionPhase::kStopping : MotionPhase::kStopped;
    motion_command_at_ = Clock::now();
    axes_unlocked_ = true;
    step_at_ = {};
    step_sent_ = false;
    pose_axes_at_ = Clock::now() + std::chrono::milliseconds(800);
    if (pending || !send_step) {
      std::printf("[运动] 停步：取消待发踏步，留在力控\n");
    }
  }
  ReleaseAxes();
  if (!send_step) return;
  SendSimple(cmd::kSteppingToggle);
  std::printf("[运动] 踏步切换 → 停步 0x21010201\n");
}

void MotionClient::ToggleStepping() {
  if (UserStepping()) StopStepping();
  else StartStepping();
}

void MotionClient::SetGait(Gait gait) {
  const uint32_t code = GaitCommandCode(gait);
  if (code == 0) return;
  std::printf("[运动] 步态 → %s 0x%08x\n", ToString(gait), code);
  SendSimple(code);
}

void MotionClient::QueueGait(Gait gait) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  queued_gait_ = gait;
  queued_gait_set_ = true;
  std::printf("[运动] 步态记下 %s（站着不发给主机）\n", ToString(gait));
}

void MotionClient::FlushQueuedGait() {
  Gait gait = Gait::kWalk;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!queued_gait_set_) return;
    gait = queued_gait_;
    queued_gait_set_ = false;
  }
  // 楼梯要地形图编排，不能只丢一条步态码。
  if (RequiresHeightMap(gait)) return;
  SetGait(gait);
}

void MotionClient::StopUnwantedMarch() {
  bool user_step = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    user_step = stepping_;
  }
  if (user_step) return;
  // 遥测常把踏步报成坐下，不能等 basic_state==踏步再停。力控把踏步切走。
  ReleaseAxes();
  EnterTorqueStand();
  std::printf("[运动] 停踏步：力控 0x2101020A\n");
}

bool MotionClient::UserStepping() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return stepping_ || state_.basic_state == BasicState::kStepping ||
         (state_.body_monitor_alive && state_.body_motion_state == 4);
}

MotionView MotionClient::View() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  MotionView out;
  // 运动 UDP 是 200Hz 控制/安全主通道；Type=1002 是低频请求响应补充。
  // 监控通道补出摔倒(7)、RL(16)，但不能覆盖 UDP emergency_source。
  const bool udp = state_.telemetry_alive;
  const bool ros =
      ros_basic_at_ != Clock::time_point{} &&
      Clock::now() - ros_basic_at_ < std::chrono::milliseconds(kRosStateFreshMs);
  const bool official = state_.body_monitor_alive &&
                        state_.body_motion_state >= 0;
  out.state_valid = udp || ros || official;
  const int body = state_.body_motion_state;
  const int ros_basic = state_.ros_basic_state;
  const bool locked =
      (udp && JointsLocked(state_.basic_state, state_.emergency_source)) ||
      (ros && ros_basic == 6) ||
      (official && (body == 6 || body == 7));
  // 官方 ROS 话题能修正现场已确认的“UDP 仍报趴下”问题。除此之外 UDP 优先。
  const bool use_ros_motion =
      ros && (!udp || (state_.basic_state == BasicState::kSitting &&
                       ros_basic != 0));
  const bool use_official_motion =
      !use_ros_motion && official &&
      (!udp || body == 16 ||
       (state_.basic_state == BasicState::kSitting &&
        body >= 1 && body <= 5));
  const int effective =
      use_ros_motion ? ros_basic
                     : (use_official_motion
                            ? body
                            : static_cast<int>(state_.basic_state));
  const bool remembered_up = last_stand_sit_ == LastStandSit::kStood;
  const bool upright =
      (use_ros_motion || use_official_motion)
          ? (effective == 2 || effective == 3 || effective == 4 ||
             effective == 16)
          : (TelemUpright(state_.basic_state) ||
             (remembered_up &&
              state_.basic_state == BasicState::kSitting));
  const bool transient = (use_ros_motion || use_official_motion)
                             ? (effective == 1 || effective == 5)
                             : IsStandSitTransient(state_.basic_state);

  if (locked) out.posture = "locked";
  else if ((use_ros_motion || use_official_motion) && effective == 1)
    out.posture = "rising";
  else if ((use_ros_motion || use_official_motion) && effective == 5)
    out.posture = "falling";
  else if (!use_ros_motion && !use_official_motion &&
           state_.basic_state == BasicState::kSitToStand)
    out.posture = "rising";
  else if (!use_ros_motion && !use_official_motion &&
           state_.basic_state == BasicState::kStandToSit)
    out.posture = "falling";
  else out.posture = upright ? "standing" : "prone";

  if (locked || !upright || transient) {
    out.phase = MotionPhase::kUnavailable;
    out.motion = "unavailable";
    return out;
  }

  const bool reported_walking =
      (use_ros_motion || use_official_motion)
          ? effective == 4
          : state_.basic_state == BasicState::kStepping;
  const bool reported_torque =
      (use_ros_motion || use_official_motion)
          ? effective == 3
          : state_.basic_state == BasicState::kTorqueStanding;
  // 本控制层刚发出的明确模式优先于滞后的主机回报。否则从 2.4G 切来时，
  // 主机还报“行走”，用户点力控/停步后 UI 仍显示行走，下一次点击又被解释成停步。
  if (motion_phase_ == MotionPhase::kStopping) {
    out.phase = reported_torque ? MotionPhase::kStopped
                                : MotionPhase::kStopping;
    out.motion = reported_torque ? "stopped" : "stopping";
  } else if (motion_phase_ == MotionPhase::kWalking) {
    out.phase = MotionPhase::kWalking;
    out.motion = "walking";
  } else if (motion_phase_ == MotionPhase::kStarting) {
    out.phase = MotionPhase::kStarting;
    out.motion = "starting";
  } else if (motion_phase_ == MotionPhase::kTorque) {
    out.phase = MotionPhase::kTorque;
    out.motion = "torque";
  } else if (motion_phase_ == MotionPhase::kStopped) {
    out.phase = MotionPhase::kStopped;
    out.motion = "stopped";
  } else if (reported_walking) {
    out.phase = MotionPhase::kWalking;
    out.motion = "walking";
  } else if (reported_torque) {
    out.phase = MotionPhase::kTorque;
    out.motion = "torque";
  } else {
    out.phase = MotionPhase::kStopped;
    out.motion = "stopped";
  }

  if (udp && out.phase == MotionPhase::kWalking) {
    out.axis_mode = "vel";
  } else if (udp && torqued_ && !stepping_ &&
             (out.phase == MotionPhase::kTorque ||
              out.phase == MotionPhase::kStopped)) {
    // 停步后的主机仍处于力控站立，四个姿态轴应继续有效。
    out.axis_mode = "pose";
  }
  return out;
}

void MotionClient::SetBodyHeight(HeightGear gear) {
  const MotionView view = View();
  if (view.phase != MotionPhase::kWalking) {
    QueueBodyHeight(gear);
    return;
  }
  SendSimple(cmd::kBodyHeight, static_cast<uint32_t>(gear));
  std::lock_guard<std::mutex> lock(state_mutex_);
  height_cmd_ = gear;
  // 遥测身高只在变化时报。发过就先改记忆，免得控制台下一帧又把打钩顶回去。
  state_.body_height_gear = (gear == HeightGear::kCrawl) ? -1 : 0;
}

void MotionClient::QueueBodyHeight(HeightGear gear) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  queued_height_ = gear;
  queued_height_set_ = true;
  std::printf("[运动] 身高记下 %s（起步后发给主机）\n",
              gear == HeightGear::kCrawl ? "匍匐" : "正常");
}

void MotionClient::ExpectGaitBeforeHeight(Gait gait) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  height_waits_for_gait_ = true;
  height_after_gait_ = gait;
}

void MotionClient::ApplyQueuedNormalHeightBeforeGait() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!queued_height_set_ || queued_height_ != HeightGear::kNormal) return;
    queued_height_set_ = false;
    height_waits_for_gait_ = false;
    height_cmd_ = HeightGear::kNormal;
    state_.body_height_gear = 0;
  }
  SendSimple(cmd::kBodyHeight, static_cast<uint32_t>(HeightGear::kNormal));
  std::printf("[运动] 步态切换前先恢复正常身高\n");
}

void MotionClient::SetControlMode(ControlMode mode) {
  SendSimple(mode == ControlMode::kManual ? cmd::kModeManual
                                          : cmd::kModeNonManual);
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (state_.control_mode != mode) {
    std::printf("[运动] 控制模式 → %s\n",
                mode == ControlMode::kManual ? "手动" : "非手动");
  }
  state_.control_mode = mode;
}

void MotionClient::SoftEmergencyStop() {
  // 急停要立刻走线，不能排在下一帧轴指令后面。同时把轴清零，
  // 免得急停解除后残留的速度指令又把机器人推出去。
  SendSimple(cmd::kSoftEmergencyStop);
  ReleaseAxes();
  std::lock_guard<std::mutex> lock(state_mutex_);
  last_stand_sit_ = LastStandSit::kSat;
  last_stand_cmd_at_ = {};
  state_.rl_standing = false;
  state_.emergency_source = 5;
  axes_unlocked_ = false;
  torqued_ = false;
  stepping_ = false;
  motion_phase_ = MotionPhase::kUnavailable;
  queued_gait_set_ = false;
  queued_height_set_ = false;
  height_waits_for_gait_ = false;
  step_sent_ = false;
  step_at_ = {};
  sit_at_ = {};
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
  // 只要起步发出去之后才收速度。力控/停步/起立都不当走路。
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!stepping_ || !step_sent_) return;
  }
  // 不要在这里冲记下的步态。杆一动就发爬坡，主机会自己踏步，再切档也停不掉。
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
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!torqued_ || stepping_) return;
  }
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
    motion_phase_ = MotionPhase::kUnavailable;
    queued_gait_set_ = false;
    queued_height_set_ = false;
    height_waits_for_gait_ = false;
    step_sent_ = false;
    step_at_ = {};
    std::printf("[运动] 本端松开：停止向运动主机发心跳，原厂手柄可单独接管\n");
  }
}

AxisView MotionClient::Axes() const {
  std::lock_guard<std::mutex> lock(axis_mutex_);
  AxisView out;
  if (Clock::now() <= axis_deadline_) {
    out.left_y = axis_left_y_;
    out.left_x = axis_left_x_;
    out.right_x = axis_right_x_;
    out.right_y = axis_right_y_;
    out.active = true;
  }
  return out;
}

RobotState MotionClient::Snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  RobotState s = state_;
  s.ros_motion_alive =
      ros_basic_at_ != Clock::time_point{} &&
      Clock::now() - ros_basic_at_ < std::chrono::milliseconds(kRosStateFreshMs);
  s.rl_standing = (last_stand_sit_ == LastStandSit::kStood);
  return s;
}

}  // namespace x30
