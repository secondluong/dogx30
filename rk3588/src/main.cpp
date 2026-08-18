// x30_gateway —— 运动网关。
//
// 两种运行模式：
//   --serve        遥控服务，对平板提供 HTTP 控制台与 WebSocket 控制通道；
//   --interactive  交互式调试终端，用来在没有平板时验证协议层的连通性。
// 交互终端保留作为排障手段，实机现场比开浏览器方便。

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "x30/motion_client.hpp"
#include "x30/robot_service.hpp"
#include "x30/terrain_client.hpp"

namespace {

using x30::ControlMode;
using x30::Gait;
using x30::HeightGear;
using x30::MotionClient;
using x30::MotionClientConfig;

// 平板遥控的本质是持续喂速度。这里用一个 20 Hz 的重复器模拟这一行为：
// 命令行设定一个目标值，重复器不停地喂，直到下一条命令改变它。
// 这样看门狗的行为跟真实场景一致 —— 一旦重复器停了，机器人就会自己停下。
class SetpointFeeder {
 public:
  explicit SetpointFeeder(MotionClient& client) : client_(client) {}

  void Start() {
    running_ = true;
    thread_ = std::thread([this] {
      while (running_) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (mode_ == Mode::kVelocity) {
            client_.SetVelocity(a_, b_, c_);
          } else if (mode_ == Mode::kPose) {
            client_.SetPose(a_, b_, c_, d_);
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
  }

  void Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

  void Velocity(float vx, float vy, float wz) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = Mode::kVelocity;
    a_ = vx;
    b_ = vy;
    c_ = wz;
    d_ = 0.0f;
  }

  void Pose(float height, float roll, float pitch, float yaw) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = Mode::kPose;
    a_ = height;
    b_ = roll;
    c_ = pitch;
    d_ = yaw;
  }

  void Idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = Mode::kIdle;
    a_ = b_ = c_ = d_ = 0.0f;
    client_.ReleaseAxes();
  }

  // 停止喂数据但不主动清零，用来验证 MotionClient 的看门狗会自己超时。
  // 这模拟的是平板掉线、上层进程卡死这类真实故障，比 Idle() 更接近实战。
  void Freeze() {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = Mode::kIdle;
  }

 private:
  enum class Mode { kIdle, kVelocity, kPose };

  MotionClient& client_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::mutex mutex_;
  Mode mode_ = Mode::kIdle;
  float a_ = 0, b_ = 0, c_ = 0, d_ = 0;
};

void PrintStatus(const MotionClient& client) {
  const auto s = client.Snapshot();
  std::printf("\n");
  if (!s.telemetry_alive) {
    std::printf("  [遥测] 未收到数据。实机上请确认已在运动主机的\n");
    std::printf("         /home/ysc/jy_exe/conf/network.toml 中登记本机 IP 与端口。\n");
    return;
  }
  std::printf("  状态      %s / %s / %s / 身高档 %d\n", x30::ToString(s.basic_state),
              x30::ToString(s.gait),
              s.control_mode == ControlMode::kManual ? "手动" : "非手动",
              s.body_height_gear);
  std::printf("  里程计    x=%.2fm y=%.2fm yaw=%.1f°\n", s.odom_x, s.odom_y,
              s.odom_yaw * 57.2958f);
  std::printf("  速度      vx=%.2f vy=%.2f wz=%.2f\n", s.vel_x, s.vel_y, s.vel_yaw);
  std::printf("  姿态      roll=%.1f° pitch=%.1f° yaw=%.1f°\n", s.roll, s.pitch,
              s.yaw);
  std::printf("  电池      %u%%  %.1fV\n", s.battery_level, s.battery_voltage);
  std::printf("  温度      CPU %.1f°C  电机峰值 %.1f°C\n", s.cpu_temperature,
              *std::max_element(s.motor_temperature, s.motor_temperature + 12));
  const auto errors = x30::DescribeErrors(s.error_state);
  if (!errors.empty()) {
    std::printf("  告警      %s\n", errors.c_str());
  }
  if (s.emergency_source != 0) {
    std::printf("  急停来源  %u\n", s.emergency_source);
  }
}

void PrintHelp() {
  std::printf(R"(
可用命令
  stand              坐 <-> 站 切换
  torque             切到力控站立（站起来之后必须先做这一步才能踏步）
  step               力控站立 <-> 踏步 切换
  gait <名字>        walk | slope | offroad | stair | stairmulti | stair45
                     | lwalk | mountain | silent
  height <档位>      normal | crawl
  mode <模式>        manual | auto
  v <vx> <vy> <wz>   踏步态下设定归一化速度，范围 [-1,1]
  p <h> <r> <p> <y>  力控站立态下设定归一化姿态，范围 [-1,1]
  stop               速度归零并主动释放轴指令
  freeze             停止喂数据但不清零，用来验证看门狗会自己超时
  estop              软急停，机器人立即趴下
  save               保存故障数据（会让运动程序退出）
  s                  打印一次状态
  watch              持续刷新状态，按回车退出
  ?                  本帮助
  q                  退出

典型流程：stand -> torque -> step -> v 0.3 0 0 -> stop -> step -> stand
)");
}

bool ParseGait(const std::string& name, Gait* out) {
  if (name == "walk") *out = Gait::kWalk;
  else if (name == "slope") *out = Gait::kSlope;
  else if (name == "offroad") *out = Gait::kOffRoad;
  else if (name == "stair") *out = Gait::kStair;
  else if (name == "stairmulti") *out = Gait::kStairMulti;
  else if (name == "stair45") *out = Gait::kStair45;
  else if (name == "lwalk") *out = Gait::kLWalk;
  else if (name == "mountain") *out = Gait::kMountain;
  else if (name == "silent") *out = Gait::kSilent;
  else return false;
  return true;
}

void RunInteractive(MotionClient& client, const MotionClientConfig& cfg) {
  SetpointFeeder feeder(client);
  feeder.Start();
  PrintHelp();

  std::string line;
  while (true) {
    std::printf("x30> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;

    std::istringstream iss(line);
    std::string verb;
    if (!(iss >> verb)) continue;

    if (verb == "q" || verb == "quit" || verb == "exit") break;
    if (verb == "?" || verb == "help") {
      PrintHelp();
    } else if (verb == "s") {
      PrintStatus(client);
    } else if (verb == "watch") {
      std::atomic<bool> watching{true};
      std::thread printer([&] {
        while (watching) {
          PrintStatus(client);
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
      });
      std::string ignored;
      std::getline(std::cin, ignored);
      watching = false;
      printer.join();
    } else if (verb == "stand") {
      client.StandOrSit();
    } else if (verb == "torque") {
      client.EnterTorqueStand();
    } else if (verb == "step") {
      client.ToggleStepping();
    } else if (verb == "gait") {
      std::string name;
      Gait gait;
      if ((iss >> name) && ParseGait(name, &gait)) {
        client.SetGait(gait);
      } else {
        std::printf("  未知步态。输入 ? 查看可用值。\n");
      }
    } else if (verb == "height") {
      std::string name;
      iss >> name;
      if (name == "crawl") {
        client.SetBodyHeight(HeightGear::kCrawl);
      } else if (name == "normal") {
        client.SetBodyHeight(HeightGear::kNormal);
      } else {
        std::printf("  身高档位只能是 normal 或 crawl。\n");
      }
    } else if (verb == "mode") {
      std::string name;
      iss >> name;
      if (name == "manual") {
        client.SetControlMode(ControlMode::kManual);
      } else if (name == "auto") {
        client.SetControlMode(ControlMode::kNonManual);
      } else {
        std::printf("  控制模式只能是 manual 或 auto。\n");
      }
    } else if (verb == "v") {
      float vx = 0, vy = 0, wz = 0;
      iss >> vx >> vy >> wz;
      const auto s = client.Snapshot();
      if (s.telemetry_alive && s.basic_state != x30::BasicState::kStepping) {
        std::printf("  当前是「%s」，不是踏步态，速度指令不会生效。\n",
                    x30::ToString(s.basic_state));
      }
      feeder.Velocity(vx, vy, wz);
    } else if (verb == "p") {
      float h = 0, r = 0, p = 0, y = 0;
      iss >> h >> r >> p >> y;
      const auto s = client.Snapshot();
      if (s.telemetry_alive &&
          s.basic_state != x30::BasicState::kTorqueStanding) {
        std::printf("  当前是「%s」，不是力控站立态，姿态指令不会生效。\n",
                    x30::ToString(s.basic_state));
      }
      feeder.Pose(h, r, p, y);
    } else if (verb == "stop") {
      feeder.Idle();
    } else if (verb == "freeze") {
      feeder.Freeze();
      std::printf("  已停止喂数据（模拟平板掉线）。看门狗应在 %d ms 内把轴指令清零。\n",
                  cfg.command_timeout_ms);
    } else if (verb == "estop") {
      feeder.Idle();
      client.SoftEmergencyStop();
      std::printf("  已发送软急停。\n");
    } else if (verb == "save") {
      client.SaveData();
      std::printf("  已发送保存数据指令，运动程序将退出。\n");
    } else {
      std::printf("  未知命令「%s」，输入 ? 查看帮助。\n", verb.c_str());
    }
  }

  feeder.Idle();
  feeder.Stop();
}

void PrintUsage() {
  std::printf(R"(用法: x30_gateway [选项]

连接:
  --robot-ip <IP>      运动主机地址，默认 192.168.1.103
  --robot-port <端口>  运动主机端口，默认 43893
  --local-port <端口>  本机接收遥测的端口，默认 43897
                       必须与运动主机 network.toml 中为本机登记的端口一致
  --perception-ip <IP> 感知主机地址，默认 192.168.1.105
  --perception-port <端口>  地形图模块端口，默认 43899
                       楼梯步态必须靠这条通道配合，不通则楼梯步态不可用

运行模式（三选一，默认为状态监视）:
  --serve              启动遥控服务：HTTP 控制台 + WebSocket，供平板接入
  --interactive        进入交互式调试终端

服务参数（配合 --serve）:
  --port <端口>        HTTP/WebSocket 监听端口，默认 8080
  --bind <地址>        监听地址，默认 0.0.0.0（全部网卡）
                       协议无身份认证，凡能连上此端口者皆可申请控制权。
                       装有 4G 等广域接口时，务必绑定到遥控链路的地址。
  --web <目录>         Web 控制台静态文件目录，默认 ./web
  --media <文件>       媒体源配置（JSON），不给则视为无视频
                       样例见 deploy/media.json，说明见 docs/media-architecture.md

点云（配合 --serve，默认关闭）:
  --cloud              启用点云下行。仅在确认感知主机 ROS 可达后再开
  --ros-master <URI>   ROS master，默认 http://192.168.1.105:11311
  --ros-host <IP>      本机在 ROS 网络中的地址，默认 192.168.1.120
                       必须是感知主机能反连的地址，也就是与狗直连的那块网卡。
                       填成 MESH 侧地址会注册成功但一条数据都收不到。
  --cloud-topic <话题> 默认 /lidar_points
  --cloud-hz <频率>    下行帧率，默认 2（话题本身是 10 Hz）
  --cloud-points <数>  单帧点数上限，默认 20000（约 120 KB/帧）

  --help               本帮助
)");
}

// Ctrl-C 时要让机器人先停下再退出，不能直接被信号打断。
std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop.store(true); }

int RunServer(MotionClient& client, x30::TerrainClient& terrain,
              const x30::RobotServiceConfig& svc_cfg) {
  x30::RobotService service(client, terrain, svc_cfg);
  std::string error;
  if (!service.Start(&error)) {
    std::fprintf(stderr, "遥控服务启动失败: %s\n", error.c_str());
    return 1;
  }

  const bool all_ifaces =
      svc_cfg.bind_address.empty() || svc_cfg.bind_address == "0.0.0.0";
  std::printf(
      "遥控服务已就绪\n"
      "  控制台   http://<本机IP>:%u/\n"
      "  WebSocket ws://<本机IP>:%u/ws\n"
      "  监听地址 %s\n"
      "  静态目录 %s\n"
      "Ctrl-C 退出。\n",
      svc_cfg.port, svc_cfg.port,
      all_ifaces ? "0.0.0.0（全部网卡）" : svc_cfg.bind_address.c_str(),
      svc_cfg.static_root.c_str());
  if (all_ifaces) {
    std::printf(
        "警告: 正在监听全部网卡，且协议无身份认证。若本机接有 4G 等广域接口，\n"
        "      请用 --bind 限定到遥控链路的地址。\n");
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::printf("\n正在停止……\n");
  client.ReleaseAxes();
  service.Stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // 作为 systemd 服务运行时 stdout 是管道，默认全缓冲，日志会攒到 4 KB 才落盘。
  // 出故障时看不到最后几条日志是致命的，改成行缓冲。
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  MotionClientConfig cfg;
  x30::TerrainClientConfig terrain_cfg;
  x30::RobotServiceConfig svc_cfg;
  bool interactive = false;
  bool serve = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? argv[++i] : std::string();
    };
    if (arg == "--robot-ip") {
      cfg.robot_ip = next();
    } else if (arg == "--robot-port") {
      cfg.robot_port = static_cast<uint16_t>(std::atoi(next().c_str()));
    } else if (arg == "--local-port") {
      cfg.local_port = static_cast<uint16_t>(std::atoi(next().c_str()));
    } else if (arg == "--perception-ip") {
      terrain_cfg.perception_ip = next();
    } else if (arg == "--perception-port") {
      terrain_cfg.perception_port =
          static_cast<uint16_t>(std::atoi(next().c_str()));
    } else if (arg == "--bind") {
      svc_cfg.bind_address = next();
    } else if (arg == "--port") {
      svc_cfg.port = static_cast<uint16_t>(std::atoi(next().c_str()));
    } else if (arg == "--web") {
      svc_cfg.static_root = next();
    } else if (arg == "--media") {
      svc_cfg.media_config = next();
    } else if (arg == "--cloud") {
      svc_cfg.cloud_enabled = true;
    } else if (arg == "--ros-master") {
      svc_cfg.cloud.master_uri = next();
    } else if (arg == "--ros-host") {
      svc_cfg.cloud.node_host = next();
    } else if (arg == "--cloud-topic") {
      svc_cfg.cloud.topic = next();
    } else if (arg == "--cloud-hz") {
      svc_cfg.cloud.cloud.target_hz = std::atoi(next().c_str());
    } else if (arg == "--cloud-points") {
      svc_cfg.cloud.cloud.max_points =
          static_cast<uint32_t>(std::atoi(next().c_str()));
    } else if (arg == "--serve") {
      serve = true;
    } else if (arg == "--interactive") {
      interactive = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::printf("未知参数: %s\n\n", arg.c_str());
      PrintUsage();
      return 1;
    }
  }

  MotionClient client(cfg);
  std::string error;
  if (!client.Start(&error)) {
    std::fprintf(stderr, "启动失败: %s\n", error.c_str());
    return 1;
  }

  std::printf("已连接 %s:%u，本机遥测端口 %u\n", cfg.robot_ip.c_str(),
              cfg.robot_port, cfg.local_port);

  // 地形图通道只写且不需要心跳，感知主机不可达也不影响其余功能，
  // 所以这里启动失败只警告，不中止 —— 不上楼梯的场景不该被它拖住。
  x30::TerrainClient terrain(terrain_cfg);
  if (!terrain.Start(&error)) {
    std::fprintf(stderr, "地形图通道启动失败（楼梯步态将不可用）: %s\n",
                 error.c_str());
  } else {
    std::printf("地形图通道 %s:%u\n", terrain_cfg.perception_ip.c_str(),
                terrain_cfg.perception_port);
  }

  int rc = 0;
  if (serve) {
    rc = RunServer(client, terrain, svc_cfg);
  } else if (interactive) {
    RunInteractive(client, cfg);
  } else {
    std::printf("状态监视模式，Ctrl-C 退出。\n");
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    while (!g_stop.load()) {
      PrintStatus(client);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  client.Stop();
  return rc;
}
