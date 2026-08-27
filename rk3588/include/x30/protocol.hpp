// X30 运动主机 UDP 协议定义。
//
// 依据《绝影 X30 API Documentation (beta) V1.0.5-0, 2025-08-21》第 1 章。
// 所有数据以小端存放。
//
// 结构体布局必须与运动主机上 jy_exe 编译出的布局逐字节一致。X30 的主机与
// RK3588 同为 64 位 Linux (LP64)，故此处刻意不加 #pragma pack，而是保留自然
// 对齐并用 static_assert 锁死 sizeof。一旦某个字段被误改，编译期就会报错。

#pragma once

#include <cstddef>
#include <cstdint>

namespace x30 {

// ---------------------------------------------------------------------------
// 报文头
// ---------------------------------------------------------------------------

// 简单指令 type = 0，此时 paramters_size 承载的是"指令值"而非长度。
// 复杂指令 type = 1，此时 paramters_size 是 data 的有效字节数。
inline constexpr uint32_t kTypeSimple = 0;
inline constexpr uint32_t kTypeComplex = 1;

struct CommandHead {
  uint32_t code;
  uint32_t paramters_size;
  uint32_t type;
};
static_assert(sizeof(CommandHead) == 12, "CommandHead 必须是 12 字节");

inline constexpr uint32_t kDataSize = 256;

struct Command {
  CommandHead head;
  uint32_t data[kDataSize];
};

// ---------------------------------------------------------------------------
// 指令码
//
// 高两位十六进制数字标识发送者身份：0x21 = 遥控终端，0x31 = 自主算法模块。
// 载荷计算机充当遥控终端，故一律用 0x21 前缀。
// ---------------------------------------------------------------------------

namespace cmd {

// 连接维持。心跳需 ≥2 Hz 持续发送，否则运动主机判定断连。
// 开始发心跳后必须补发一次 kConnectConfirm。
inline constexpr uint32_t kHeartbeat = 0x21040001;
inline constexpr uint32_t kConnectConfirm = 0x21020001;

// 基础状态迁移
//
// 文档只写了 0x21010202 这一条切换。现场对照运动主机日志：
//   原厂手柄起立 → "Received RL-StandUp"     0x21010223
//   原厂手柄趴下 → "Received RL Sit Cmd"     0x21010222
//   我们发 0x21010202 → "Received stand up or sit down command"
// 后一条是旧路径，起身/趴下又快又硬；前两条走 RL 策略，才是遥控器那种柔和轨迹。
// 这两码不在公开 API 里，但是运动程序认、原厂手柄在用，比文档更值得信。
// RL 起立后 basic_state 仍报 0，下一次该起还是该坐只能自己记。
//
// 0x21010202 日常不再用来起/趴。软急停后关节自锁，RL 起/趴会被主机吞掉；
// 原厂 App 的「卸力」、手册里急停后按 ⑤/㉑，打的都是这条。现场 8/10
// 硬件急停松开后先 0x21010202（"Robot is waitting to stand up"），再 RL 起立。
inline constexpr uint32_t kStandSitToggle = 0x21010202;  // 旧切换；急停后卸力
inline constexpr uint32_t kUnloadForce = kStandSitToggle;
inline constexpr uint32_t kRlSitDown = 0x21010222;       // 原厂手柄趴下
inline constexpr uint32_t kRlStandUp = 0x21010223;       // 原厂手柄起立
inline constexpr uint32_t kTorqueStand = 0x2101020A;     // 初始站立 -> 力控站立
inline constexpr uint32_t kSteppingToggle = 0x21010201;  // 力控站立 <-> 踏步 切换

// 控制模式。非手动模式下机器人执行经地形图模块修正后的速度指令。
inline constexpr uint32_t kModeNonManual = 0x21010C03;
inline constexpr uint32_t kModeManual = 0x21010C02;

// 身高档位，指令值 0 = 匍匐，2 = 正常
inline constexpr uint32_t kBodyHeight = 0x21010406;

// 软急停：立即趴下并进入关节保护
inline constexpr uint32_t kSoftEmergencyStop = 0x21010C0E;

// 保存故障前 5 分钟数据并退出运动程序。jy_exe < v2.2.45 时改用 kSaveDataLegacy。
inline constexpr uint32_t kSaveData = 0x010C01;
inline constexpr uint32_t kSaveDataLegacy = 18;

// 步态。仅在踏步状态下可切换。
// 三种楼梯步态必须配合对应的地形图模式才生效。
inline constexpr uint32_t kGaitWalk = 0x21010300;
inline constexpr uint32_t kGaitSlope = 0x21010402;
inline constexpr uint32_t kGaitOffRoad = 0x21010401;
inline constexpr uint32_t kGaitStair = 0x21010405;       // 地形图 = 实心/格栅/无踢面
inline constexpr uint32_t kGaitStairMulti = 0x2101040A;  // 地形图 = 多帧
inline constexpr uint32_t kGaitStair45 = 0x2101040B;     // 地形图 = 多帧
inline constexpr uint32_t kGaitLWalk = 0x21010420;
inline constexpr uint32_t kGaitMountain = 0x21010421;
inline constexpr uint32_t kGaitSilent = 0x21010422;

// 轴指令。取值 [-32767, 32767]，需 50 Hz 持续发送，超过 1 秒未收到即失效。
// 同一组指令码在不同基础状态下语义不同：
//   力控站立态 -> 调姿态；踏步态 -> 调速度。
inline constexpr uint32_t kAxisLeftY = 0x21010130;   // 站立: 身高    踏步: X 向线速度
inline constexpr uint32_t kAxisLeftX = 0x21010131;   // 站立: 横滚    踏步: Y 向线速度
inline constexpr uint32_t kAxisRightX = 0x21010135;  // 站立: 偏航    踏步: 偏航角速度
inline constexpr uint32_t kAxisRightY = 0x21010102;  // 站立: 俯仰    踏步: 无

}  // namespace cmd

// ---------------------------------------------------------------------------
// 地形图模块指令码（发往感知主机 192.168.1.105:43899）
//
// 与运动主机是**两个不同的主机和端口**。楼梯步态必须配合对应的地形图模式，
// 只发步态指令运动主机会静默忽略，不报错。
//
// 前缀 0x31 表示发送者是自主算法模块，与运动指令的 0x21 不同，取自 API 文档。
//
// 这条通道是只写的：模块不回确认报文。设置是否生效只能通过运动主机回传的
// 步态遥测间接判断 —— 地形图模式不对时步态切不过去，正好可以拿来当确认。
// ---------------------------------------------------------------------------

namespace terrain {

inline constexpr uint32_t kHeightMapMode = 0x3101EE01;
inline constexpr uint32_t kBrakeMode = 0x3101EE02;
inline constexpr uint32_t kVelSource = 0x3101EE03;
inline constexpr uint32_t kStepZMax = 0x3100EE04;

// 原厂 fast-lio。发往感知主机 :60000，不是地形图的 43899。
// 1=开 0=关。狗要站稳再开，API 1.5.2.4 / 附录 C。
inline constexpr uint32_t kLioToggle = 0x0BAA0001;
inline constexpr uint16_t kLioPort = 60000;

}  // namespace terrain

// 地形图模式。三种单帧模式按楼梯的实际构造选，多帧用于连续爬升。
enum class HeightMapMode : uint32_t {
  kSolid = 3,            // 实心踏面
  kGrating = 4,          // 格栅踏面
  kNoRiser = 5,          // 无踢面
  kMultiFramePrep = 18,  // 多帧准备
  kMultiFrame = 20,      // 多帧
};

enum class VelSource : uint32_t {
  kHandle = 1,
  kNavigation = 2,
};

enum class BrakeMode : uint32_t {
  kSlowDown = 1,  // 减速
  kBypass = 2,    // 绕行
};

// 障碍高度阈值。文档推荐 Walk/缓坡 8cm，越野/楼梯 28cm。
enum class StepZMax : uint32_t {
  k8cm = 1,
  k28cm = 2,
};

const char* ToString(HeightMapMode m);

// 轴指令取值范围与死区。|v| < 655 时机器人视作零输入。
inline constexpr int32_t kAxisMax = 32767;
inline constexpr int32_t kAxisDeadZone = 655;

// ---------------------------------------------------------------------------
// 遥测报文码
//
// 运动主机只向 network.toml 中登记过的 IP:Port 单播这些报文。
// ---------------------------------------------------------------------------

namespace telem {

inline constexpr uint32_t kRunningStatus = 0x1008;     // RcsData,               200 Hz
inline constexpr uint32_t kMotionStatus = 0x1009;      // MotionStateData,       200 Hz
inline constexpr uint32_t kSensorData = 0x100A;        // ControllerSensorData,  200 Hz
inline constexpr uint32_t kSafeData = 0x100B;          // ControllerSafeData,      1 Hz
inline constexpr uint32_t kBattery = 0x21050F0A;       // BatterySensorData,     0.5 Hz
inline constexpr uint32_t kBodyHeightState = 0x11050F08;  // 简单报文, -1=匍匐 0=正常

}  // namespace telem

// ---------------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------------

enum class BasicState : uint8_t {
  kSitting = 0,
  kSitToStand = 1,
  kInitialStanding = 2,
  kTorqueStanding = 3,
  kStepping = 4,
  kStandToSit = 5,
  kEmergencyOrFall = 6,
};

// 坐↔站过渡。再发一次切换会打断正在走的轨迹，甚至当场反转。
inline bool IsStandSitTransient(BasicState s) {
  return s == BasicState::kSitToStand || s == BasicState::kStandToSit;
}

// 关节自锁。急停后 basic_state 常改回报坐下，原厂还看 0x1008 的来源字节。
//
// 只认明确的锁：状态 6，或充电/客户端/RAS（4–6）。
// 1 和 is_nav_mode 只差一字节，切楼梯会误读。2/3 是运动/导航里的自主来源，
// 切步态、进非手动时主机常带上，不是急停；当成锁就会把趴下改成卸力，
// 狗还在踏步却趴不下。真急停会同时报 basic_state=6。
inline bool JointsLocked(BasicState s, uint8_t emergency_source = 0) {
  if (s == BasicState::kEmergencyOrFall) return true;
  return emergency_source >= 4 && emergency_source <= 6;
}

// 轴指令只在力控站立 / 踏步有文档定义（API 1.2.3）。
//   力控站立：身高 / 横滚 / 俯仰 / 偏航
//   踏步：速度；俯仰轴无定义
// 初始站立或 RL 起立后撒谎的「坐下」里发轴，主机会按速度理解：
// 力控左杆变成走路，右杆 Y 的俯仰被丢掉。起立中 / 坐下中 / 急停不发，
// 免得把柔和起身掐硬。standing 留给调用方，这里不再凭「记得站着」放行。
inline bool AxisCommandsApply(BasicState s, bool standing = false,
                             uint8_t emergency_source = 0) {
  (void)standing;
  if (JointsLocked(s, emergency_source)) return false;
  if (IsStandSitTransient(s)) return false;
  return s == BasicState::kTorqueStanding || s == BasicState::kStepping;
}

// 运动主机自己报的站立态。不含「我们记得 RL 已起立、遥测仍报坐下」。
inline bool TelemUpright(BasicState s) {
  return s == BasicState::kInitialStanding ||
         s == BasicState::kTorqueStanding ||
         s == BasicState::kStepping;
}

// 原厂 LIO 要求站稳再开。RL 起立后遥测仍报坐下，要看 rl_standing。
// 起立中 / 坐下中身子在动，不开。
inline bool StandingForLio(BasicState s, bool rl_standing = false,
                          uint8_t emergency_source = 0) {
  if (IsStandSitTransient(s) || JointsLocked(s, emergency_source)) {
    return false;
  }
  if (TelemUpright(s)) return true;
  return rl_standing && s == BasicState::kSitting;
}

// 步态指令主机文档写「仅踏步态」。RL 起立后遥测仍报坐下，编排器若只看
// basic_state，顶栏已经是「RL 站立」时切步态还会报「当前为坐下」。
// 力控站立 / 初始站立 / 我们记得 RL 已起立，都放行。真趴着才挡。
inline bool GaitSwitchApply(BasicState s, bool rl_standing = false,
                           uint8_t emergency_source = 0) {
  if (IsStandSitTransient(s) || JointsLocked(s, emergency_source)) {
    return false;
  }
  if (TelemUpright(s)) return true;
  return rl_standing && s == BasicState::kSitting;
}

enum class Gait : uint8_t {
  kWalk = 0,
  kOffRoad = 1,
  kSlope = 2,
  kRun = 3,
  kStair = 6,
  kStairMulti = 7,
  kStair45 = 8,
  kLWalk = 32,
  kMountain = 33,
  kSilent = 34,
};

enum class ControlMode : uint8_t {
  kManual = 0,
  kNonManual = 1,
};

enum class HeightGear : uint32_t {
  kCrawl = 0,
  kNormal = 2,
};

const char* ToString(BasicState s);
const char* ToString(Gait g);

// 把步态枚举翻译成对应的指令码。地形图模式需调用方另行保证。
uint32_t GaitCommandCode(Gait g);

// 各步态的最大速度，用于把归一化输入换算成物理量以便显示。
// 数据取自 API 文档附录 B，正常身高档位。匍匐档下 Walk/Slope 的前向速度减半。
struct GaitLimits {
  float max_forward_mps;
  float max_lateral_mps;
  float max_yaw_radps;
};
GaitLimits LimitsOf(Gait g);

// 该步态推荐的障碍高度阈值。切步态时一并跟着改，
// 否则上楼梯会被 8cm 的阈值当成障碍挡住。
StepZMax RecommendedStepZMax(Gait g);

// 是否为需要地形图模块配合的楼梯步态。
bool RequiresHeightMap(Gait g);

// 该楼梯步态要求的地形图模式是否为多帧。多帧只能在机器人静止时切换。
bool RequiresMultiFrame(Gait g);

// ---------------------------------------------------------------------------
// 遥测数据结构
// ---------------------------------------------------------------------------

// 0x1008 机器人运行状态
struct RcsData {
  char robot_name[15];
  int32_t current_mileage;  // 本次开机里程 (cm)
  int32_t total_mileage;    // 累计里程 (cm)
  // 文档里这四个字段声明为 long。运动主机是 LP64 Linux，long 即 8 字节，
  // 这里写死 int64_t 以免在 LLP64 (Windows) 上交叉编译时布局漂移。
  int64_t current_run_time;  // 本次开机运行时间 (s)
  int64_t total_run_time;
  int64_t current_motion_time;  // 本次开机行走时间 (s)
  int64_t total_motion_time;
  float joystick[4];  // 轴指令值，已缩放到 [-1.0, 1.0]，顺序 LX LY RX RY
  union {
    struct {
      uint8_t is_nav_mode;       // 0 = 手动, 1 = 非手动
      uint8_t emergency_source;  // 软急停来源，见文档 1.3.1
      uint8_t mode_reserved[8];
    } rcs_state_list;
    uint8_t rcs_state[10];
  };
  union {
    struct {
      uint32_t imu_error : 1;
      uint32_t wifi_error : 1;  // 心跳超时
      uint32_t driver_heat_warn : 1;
      uint32_t driver_error : 1;
      uint32_t motor_heat_warn : 1;
      uint32_t battery_low_warn : 1;
      uint32_t battery_heat_warn : 1;
      uint32_t gpio_error : 1;
      uint32_t cpu_heat_warn : 1;
      uint32_t cpu_freq_warn : 1;
      uint32_t reserved : 22;
    } error_state_bit;
    uint32_t error_state;
  };
};
static_assert(sizeof(RcsData) == 88, "RcsData 布局与运动主机不一致");

// 0x1009 机器人运动状态
struct MotionStateData {
  uint8_t basic_state;
  uint8_t gait_state;
  float max_forward_vel;   // 已废弃
  float max_backward_vel;  // 已废弃
  float leg_odom_pos[3];   // 世界系 {x(m), y(m), yaw(rad)}，原点为开机位置
  float leg_odom_vel[3];   // 机体系 {vx(m/s), vy(m/s), wz(rad/s)}
  float robot_distance;    // 本次开机里程 (cm)
  uint32_t touch_state;    // 保留
  union {
    struct {
      uint32_t narrow_walk : 1;
      uint32_t pose_safe_flag : 1;
      uint32_t joint_limit_flag : 1;
      uint32_t state_reserved : 29;
    } control_state_bit;
    uint32_t control_state;  // 保留
  };
  union {
    struct {
      uint8_t auto_charge_state;  // 已废弃
      uint8_t pos_ctrl_state;     // 位置控制状态，见文档 1.3.2
      uint8_t task_reserved[8];
    } task_state_list;
    uint8_t task_state[10];
  };
};
static_assert(sizeof(MotionStateData) == 60, "MotionStateData 布局与运动主机不一致");

struct ImuSensorData {
  int32_t timestamp;
  union {
    float buffer_float[9];
    uint8_t buffer_byte[3][12];
    struct {
      float roll, pitch, yaw;              // 角度 (°)
      float omega_x, omega_y, omega_z;     // 角速度 (rad/s)
      float acc_x, acc_y, acc_z;           // 加速度 (m/s^2)
    };
  };
};
static_assert(sizeof(ImuSensorData) == 40, "ImuSensorData 布局与运动主机不一致");

// 12 个关节顺序固定为 FL / FR / HL / HR，每条腿依次 HipX(侧摆) HipY(前摆) Knee(膝)。
struct LegJointData {
  union {
    float data[12];
    struct {
      float fl_hipx_value, fl_hipy_value, fl_knee_value;
      float fr_hipx_value, fr_hipy_value, fr_knee_value;
      float hl_hipx_value, hl_hipy_value, hl_knee_value;
      float hr_hipx_value, hr_hipy_value, hr_knee_value;
    };
  };
};
static_assert(sizeof(LegJointData) == 48, "LegJointData 布局与运动主机不一致");

// 0x100A 运动控制传感器数据
struct ControllerSensorData {
  ImuSensorData imu_data;
  LegJointData joint_pos;  // rad
  LegJointData joint_vel;  // rad/s
  LegJointData joint_tau;  // N·m
};
static_assert(sizeof(ControllerSensorData) == 184,
              "ControllerSensorData 布局与运动主机不一致");

struct CpuInfo {
  float temperature;  // ℃
  float frequency;    // MHz
};

// 0x100B 运动控制系统状态
struct ControllerSafeData {
  float motor_temperature[12];    // ℃
  uint8_t driver_temperture[12];  // ℃，官方拼写如此
  CpuInfo cpu_info_;
};
static_assert(sizeof(ControllerSafeData) == 68,
              "ControllerSafeData 布局与运动主机不一致");

// 0x21050F0A 电池信息
struct BatterySensorData {
  uint16_t voltage;  // V
  int16_t current;   // jy_exe >= 1.10.19 时单位为 10mA，更早版本为 A
  uint16_t remaining_capacity;  // 10mAh
  uint16_t nominal_capacity;    // 10mAh
  uint16_t cycles;
  uint16_t production_date;
  uint16_t balanced_low;
  uint16_t balanced_high;
  uint16_t protected_state;
  uint8_t software_version;
  uint8_t battery_level;  // %
  uint8_t mos_state;
  uint8_t battery_quantity;
  uint8_t battery_ntc;
  float battery_temperature[4];  // ℃
};
static_assert(sizeof(BatterySensorData) == 40,
              "BatterySensorData 布局与运动主机不一致");

}  // namespace x30
