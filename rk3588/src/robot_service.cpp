#include "x30/robot_service.hpp"

#include <algorithm>
#include <cstdio>

#include "x30/json.hpp"

namespace x30 {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kVersion = "0.2.0";

// ToString(Gait) 返回的是中文显示名，不能拿来做标识比较。遥控端需要一个
// 稳定的机器可读键来高亮当前步态按钮，这里给出与 ParseGaitName 互逆的映射。
const char* GaitKey(Gait g) {
  switch (g) {
    case Gait::kWalk: return "walk";
    case Gait::kSlope: return "slope";
    case Gait::kOffRoad: return "offroad";
    case Gait::kRun: return "run";
    case Gait::kStair: return "stair";
    case Gait::kStairMulti: return "stairmulti";
    case Gait::kStair45: return "stair45";
    case Gait::kLWalk: return "lwalk";
    case Gait::kMountain: return "mountain";
    case Gait::kSilent: return "silent";
  }
  return "unknown";
}

bool ParseGaitName(const std::string& name, Gait* out) {
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

// 单帧楼梯步态要按实际楼梯的踏面构造选地形图模式。缺省按实心处理 ——
// 这是最常见的混凝土楼梯，选错了表现为步态切不过去，不会有安全风险。
bool ParseStairStyle(const std::string& name, HeightMapMode* out) {
  if (name.empty() || name == "solid") *out = HeightMapMode::kSolid;
  else if (name == "grating") *out = HeightMapMode::kGrating;
  else if (name == "noriser") *out = HeightMapMode::kNoRiser;
  else return false;
  return true;
}

float Clamp01(double v) {
  if (v > 1.0) return 1.0f;
  if (v < -1.0) return -1.0f;
  return static_cast<float>(v);
}

}  // namespace

RobotService::RobotService(MotionClient& client, TerrainClient& terrain,
                           RobotServiceConfig config)
    : client_(client),
      terrain_(terrain),
      cfg_(std::move(config)),
      gaits_(client, terrain, GaitCoordinatorConfig{}) {}

RobotService::~RobotService() { Stop(); }

bool RobotService::Start(std::string* error) {
  if (running_.load()) return true;

  // 媒体配置加载失败不阻止启动。视频是附加能力，控制才是本体 ——
  // 为了一个配错的相机地址让整个网关起不来是本末倒置。
  if (!cfg_.media_config.empty()) {
    MediaConfig mc;
    std::string media_err;
    if (LoadMediaConfig(cfg_.media_config, &mc, &media_err)) {
      std::printf("媒体源 %zu 路，出口 %s\n", mc.sources.size(),
                  mc.webrtc_base.c_str());
      media_ = std::make_unique<MediaRegistry>(std::move(mc));
    } else {
      std::fprintf(stderr, "媒体配置未加载（视频不可用）: %s\n",
                   media_err.c_str());
    }
  }

  // 同理，点云也是附加能力。这里只是创建桥接对象，真正去连感知主机要等到
  // 有遥控端订阅为止 —— 没人看的时候不该在机器狗的 ROS 上挂一个订阅者。
  if (cfg_.cloud_enabled) {
    cloud_ = std::make_unique<CloudBridge>(cfg_.cloud, server_);
    std::printf("点云桥接已就绪：master %s，话题 %s\n",
                cfg_.cloud.master_uri.c_str(), cfg_.cloud.topic.c_str());
  }

  server_.SetStaticRoot(cfg_.static_root);
  server_.SetHandlers([this](WsServer::ClientId id) { OnConnect(id); },
                      [this](WsServer::ClientId id, const std::string& text) {
                        OnMessage(id, text);
                      },
                      [this](WsServer::ClientId id) { OnDisconnect(id); });

  if (!server_.Start(cfg_.bind_address, cfg_.port, error)) return false;

  gaits_.Start();
  if (cloud_) cloud_->Start();
  running_.store(true);
  state_thread_ = std::thread(&RobotService::StateLoop, this);
  return true;
}

void RobotService::Stop() {
  if (!running_.exchange(false)) return;
  if (state_thread_.joinable()) state_thread_.join();
  if (cloud_) cloud_->Stop();
  gaits_.Stop();
  server_.Stop();
}

// ---------------------------------------------------------------------------
// 控制权
// ---------------------------------------------------------------------------

bool RobotService::HoldsControl(WsServer::ClientId id) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  return controller_ == id && Clock::now() <= lease_expiry_;
}

bool RobotService::TryClaim(WsServer::ClientId id) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  const bool free = controller_ == 0 || controller_ == id ||
                    Clock::now() > lease_expiry_;
  if (!free) return false;
  controller_ = id;
  lease_expiry_ = Clock::now() + std::chrono::milliseconds(cfg_.control_lease_ms);
  client_.SetCommanding(true);
  return true;
}

void RobotService::ReleaseControl(WsServer::ClientId id, bool zero_axes) {
  bool released = false;
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (controller_ == id) {
      controller_ = 0;
      lease_expiry_ = Clock::time_point{};
      released = true;
    }
  }
  if (released && zero_axes) {
    // 控制端走了就立刻停车，不等看门狗那 300 ms。
    client_.ReleaseAxes();
  }
  if (released) {
    client_.SetCommanding(false);
    BroadcastControlState();
  }
}

void RobotService::TouchLease(WsServer::ClientId id) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  // 已经过期就不再续。过期意味着控制权已经放出去了，让它走正常的重新 claim 流程，
  // 否则一条迟到的消息会把刚被别人接管的控制权抢回来。
  if (controller_ != id || Clock::now() > lease_expiry_) return;
  lease_expiry_ = Clock::now() + std::chrono::milliseconds(cfg_.control_lease_ms);
}

void RobotService::BroadcastControlState() {
  WsServer::ClientId holder;
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    holder = controller_;
  }
  JsonWriter w;
  w.BeginObject()
      .Key("t", "control")
      .Key("holder", static_cast<int>(holder))
      .EndObject();
  server_.Broadcast(w.Take());
}

void RobotService::SendError(WsServer::ClientId id, const char* code,
                             const char* msg) {
  JsonWriter w;
  w.BeginObject().Key("t", "error").Key("code", code).Key("msg", msg).EndObject();
  server_.Send(id, w.Take());
}

// 步态切换是异步的，结果单独回一条。成功也要回 —— 楼梯切换可能要几秒，
// 操作员需要知道到底成没成，而不是盯着按钮猜。
void RobotService::SendGaitResult(WsServer::ClientId id, Gait target,
                                  const GaitCoordinator::Result& result) {
  JsonWriter w;
  w.BeginObject()
      .Key("t", "gait_result")
      .Key("gait_key", GaitKey(target))
      .Key("ok", result.ok)
      .Key("code", result.code)
      .Key("msg", result.message)
      .EndObject();
  server_.Send(id, w.Take());

  if (!result.ok) {
    std::printf("[ws] 客户端 %llu 切换到 %s 失败: %s\n",
                static_cast<unsigned long long>(id), ToString(target),
                result.message.c_str());
  }
}

// ---------------------------------------------------------------------------
// 在线改配置
// ---------------------------------------------------------------------------

bool RobotService::ConfigEnabled() const { return !cfg_.config_path.empty(); }

bool RobotService::ControlHeld() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  return controller_ != 0 && Clock::now() <= lease_expiry_;
}

bool RobotService::CheckAdminToken(WsServer::ClientId id, const Json& msg) {
  const std::string expected = LoadAdminToken(cfg_.admin_token_file);
  if (expected.empty()) {
    SendError(id, "no_admin_token",
              "本机没有配置管理令牌，在线改配置已禁用。"
              "重跑 deploy/install.sh 会生成一个。");
    return false;
  }
  if (!TokenMatches(expected, msg.String("token"))) {
    // 敏感操作的失败要留痕。现场若真有人在试，日志是唯一的线索。
    std::printf("[配置] 客户端 %llu 管理令牌不符，已拒绝\n",
                static_cast<unsigned long long>(id));
    SendError(id, "bad_admin_token", "管理令牌不正确");
    return false;
  }
  return true;
}

void RobotService::HandleConfigGet(WsServer::ClientId id) {
  JsonWriter w;
  w.BeginObject()
      .Key("t", "config")
      .Raw("settings", GatewaySettingsJson(cfg_.settings))
      .Key("path", cfg_.config_path)
      // 遥控端据此决定保存后是「等它自己回来」还是「提示人去重启」。
      .Key("auto_restart", static_cast<bool>(cfg_.request_restart))
      .Key("control_held", ControlHeld())
      .EndObject();
  server_.Send(id, w.Take());
}

void RobotService::HandleConfigSet(WsServer::ClientId id, const Json& msg) {
  // 改完要重启，重启会中断遥控几秒。狗正被人操控时绝不能发生。
  if (ControlHeld()) {
    SendError(id, "busy_control",
              "有客户端正持有控制权。改配置需要重启网关，遥控会中断，"
              "请先释放控制权。");
    return;
  }

  GatewaySettings next = cfg_.settings;
  std::string error;
  if (!MergeGatewaySettings(msg["settings"], &next, &error)) {
    SendError(id, "bad_config", error.c_str());
    return;
  }
  if (!ValidateGatewaySettings(next, cfg_.settings, &error)) {
    SendError(id, "bad_config", error.c_str());
    return;
  }
  if (!SaveGatewaySettings(cfg_.config_path, next, &error)) {
    SendError(id, "config_write_failed", error.c_str());
    return;
  }

  cfg_.settings = next;
  std::printf("[配置] 客户端 %llu 已写入新配置：运动主机 %s，感知主机 %s，"
              "监听 %s:%u，点云 %s\n",
              static_cast<unsigned long long>(id), next.robot_ip.c_str(),
              next.perception_ip.c_str(), next.bind_address.c_str(),
              next.http_port, next.cloud_enabled ? "开" : "关");

  const bool auto_restart = static_cast<bool>(cfg_.request_restart);
  JsonWriter w;
  w.BeginObject()
      .Key("t", "config_saved")
      .Raw("settings", GatewaySettingsJson(next))
      .Key("auto_restart", auto_restart)
      .EndObject();
  server_.Send(id, w.Take());

  if (!auto_restart) return;

  // 让回执先出门再退出。直接在这里请求停机的话，服务器会在响应写完之前就
  // 开始收摊，遥控端只看到连接断开，不知道到底存没存上。
  //
  // 拷一份 std::function 而不是捕获 this：这个线程的寿命可能超过本对象。
  auto restart = cfg_.request_restart;
  std::thread([restart]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    restart();
  }).detach();
}

// ---------------------------------------------------------------------------
// 连接与消息
// ---------------------------------------------------------------------------

void RobotService::OnConnect(WsServer::ClientId id) {
  WsServer::ClientId holder;
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    holder = controller_;
  }
  JsonWriter w;
  w.BeginObject()
      .Key("t", "hello")
      .Key("version", kVersion)
      .Key("client_id", static_cast<int>(id))
      .Key("control", holder == id)
      .Key("holder", static_cast<int>(holder))
      .Key("lease_ms", cfg_.control_lease_ms)
      // 本机是否支持在线改配置。只是个能力位，不含任何配置内容 ——
      // 遥控端据此决定要不要显示「设置」入口，真要取值还得凭令牌。
      .Key("config", ConfigEnabled())
      .EndObject();
  server_.Send(id, w.Take());
  server_.Send(id, BuildStateJson());

  // 先按「只支持 H.264」下发一份计划，遥控端上报 media_caps 后再更新。
  // 这样它连上就有画面可看，不用等能力探测走完。
  if (media_) SendMediaPlan(id);

  std::printf("[ws] 客户端 %llu 已连接（在线 %zu）\n",
              static_cast<unsigned long long>(id), server_.ClientCount());
}

void RobotService::OnDisconnect(WsServer::ClientId id) {
  ReleaseControl(id, /*zero_axes=*/true);

  // 断开的客户端可能正占着全码率槽位，要还回去，否则这一路就永久废了。
  if (media_) {
    const bool had_slot = media_->full_quality_holder() == id;
    media_->Forget(id);
    if (had_slot) {
      for (const auto other : server_.ClientIds()) SendMediaPlan(other);
    }
  }

  // 不退订的话，最后一个遥控端断开后点云会一直在链路上跑。
  if (cloud_) cloud_->RemoveSubscriber(id);

  std::printf("[ws] 客户端 %llu 已断开（在线 %zu）\n",
              static_cast<unsigned long long>(id), server_.ClientCount());
}

void RobotService::SendMediaPlan(WsServer::ClientId id) {
  if (!media_) return;
  server_.Send(id, media_->BuildPlanJson(id));
}

void RobotService::OnMessage(WsServer::ClientId id, const std::string& text) {
  bool ok = false;
  const Json msg = Json::Parse(text, &ok);
  if (!ok || msg.type() != Json::Type::kObject) {
    SendError(id, "bad_request", "JSON 解析失败");
    return;
  }

  const std::string t = msg.String("t");

  TouchLease(id);

  if (t == "ping") {
    JsonWriter w;
    w.BeginObject()
        .Key("t", "pong")
        .Key("id", static_cast<int>(msg.Number("id")))
        .EndObject();
    server_.Send(id, w.Take());
    return;
  }

  if (t == "claim") {
    const bool granted = TryClaim(id);
    JsonWriter w;
    w.BeginObject().Key("t", "control").Key("granted", granted);
    {
      std::lock_guard<std::mutex> lock(control_mutex_);
      w.Key("holder", static_cast<int>(controller_));
    }
    w.EndObject();
    server_.Send(id, w.Take());
    if (granted) BroadcastControlState();
    return;
  }

  if (t == "yield") {
    ReleaseControl(id, /*zero_axes=*/true);
    return;
  }

  // 点云同样不检查控制权：看是所有人的权利，操控才是一个人的。
  if (t == "cloud_sub" || t == "cloud_unsub") {
    if (!cloud_) {
      SendError(id, "no_cloud", "本机未启用点云（需要 --cloud 并确认感知主机可达）");
      return;
    }
    if (t == "cloud_sub") {
      cloud_->AddSubscriber(id);
    } else {
      cloud_->RemoveSubscriber(id);
    }
    server_.Send(id, cloud_->StatusJson());
    return;
  }

  // 媒体相关的消息**不检查控制权**：围观视频是正常需求，不该和操控权绑定。
  // 带宽约束由 MediaRegistry 单独强制，与控制权是两套机制。
  if (t == "media_caps" || t == "media_select" || t == "media_plan") {
    if (!media_) {
      SendError(id, "no_media", "本机未配置视频源");
      return;
    }
    if (t == "media_caps") {
      // 默认按不支持 H.265 处理。旧 WebView 没有 H.265 软解兜底，
      // 猜错的代价是黑屏，所以只认遥控端实测上报的结果。
      media_->SetCaps(id, msg["h264"].AsBool(true), msg["h265"].AsBool(false));
      SendMediaPlan(id);
      return;
    }
    if (t == "media_select") {
      std::string reason;
      const bool full = media_->Select(id, msg.String("id"), &reason);
      if (!full && !reason.empty()) {
        SendError(id, "media_degraded", reason.c_str());
      }
      // 全码率槽位是全局的，一个人选完别人的计划也可能变，所以都要重发。
      for (const auto other : server_.ClientIds()) SendMediaPlan(other);
      return;
    }
    SendMediaPlan(id);
    return;
  }

  // 改配置要单独的管理令牌，控制权在这里不作数：操控狗和改网关指向是两回事，
  // 后者危险得多（能把服务指到别的主机上，也能把监听面从内网扩到全部网卡）。
  if (t == "config_get" || t == "config_set") {
    if (!ConfigEnabled()) {
      SendError(id, "no_config",
                "本机未启用在线改配置（需要以 --config 指定配置文件）");
      return;
    }
    if (!CheckAdminToken(id, msg)) return;
    if (t == "config_get") {
      HandleConfigGet(id);
    } else {
      HandleConfigSet(id, msg);
    }
    return;
  }

  if (t == "cmd") {
    const std::string name = msg.String("name");

    // 急停不检查控制权。安全动作绝不能因为权限判断而延迟或被拒。
    if (name == "estop") {
      client_.SoftEmergencyStop();
      std::printf("[ws] 客户端 %llu 触发软急停\n",
                  static_cast<unsigned long long>(id));
      return;
    }

    if (!HoldsControl(id)) {
      SendError(id, "no_control", "未持有控制权，请先发送 claim");
      return;
    }

    if (name == "stand") {
      client_.StandOrSit();
    } else if (name == "torque") {
      client_.EnterTorqueStand();
    } else if (name == "step") {
      client_.ToggleStepping();
    } else if (name == "savedata") {
      client_.SaveData();
    } else if (name == "gait") {
      Gait gait;
      if (!ParseGaitName(msg.String("value"), &gait)) {
        SendError(id, "bad_request", "未知步态");
        return;
      }
      // 楼梯步态要跨两台主机按序设置并等待确认，耗时可达数秒，不能在这里阻塞
      // —— 消息线程卡住会连带堵掉急停。交给编排器异步执行，结果回推给发起方。
      HeightMapMode style = HeightMapMode::kSolid;
      if (!ParseStairStyle(msg.String("stair_style"), &style)) {
        SendError(id, "bad_request", "未知踏面类型");
        return;
      }
      const bool accepted = gaits_.Request(
          gait, style, [this, id, gait](const GaitCoordinator::Result& r) {
            SendGaitResult(id, gait, r);
          });
      if (!accepted) {
        SendError(id, "gait_busy", "上一次步态切换尚未完成");
        return;
      }
    } else if (name == "height") {
      const std::string v = msg.String("value");
      if (v == "crawl") {
        client_.SetBodyHeight(HeightGear::kCrawl);
      } else if (v == "normal") {
        client_.SetBodyHeight(HeightGear::kNormal);
      } else {
        SendError(id, "bad_request", "身高档位只能是 normal 或 crawl");
        return;
      }
    } else if (name == "mode") {
      const std::string v = msg.String("value");
      if (v == "manual") {
        client_.SetControlMode(ControlMode::kManual);
      } else if (v == "auto") {
        client_.SetControlMode(ControlMode::kNonManual);
      } else {
        SendError(id, "bad_request", "控制模式只能是 manual 或 auto");
        return;
      }
    } else {
      SendError(id, "unknown_command", "未知指令");
      return;
    }
    return;
  }

  if (t == "vel" || t == "pose" || t == "release") {
    if (!HoldsControl(id)) {
      SendError(id, "no_control", "未持有控制权，请先发送 claim");
      return;
    }
    if (t == "vel") {
      client_.SetVelocity(Clamp01(msg.Number("vx")), Clamp01(msg.Number("vy")),
                          Clamp01(msg.Number("wz")));
    } else if (t == "pose") {
      client_.SetPose(Clamp01(msg.Number("h")), Clamp01(msg.Number("roll")),
                      Clamp01(msg.Number("pitch")), Clamp01(msg.Number("yaw")));
    } else {
      client_.ReleaseAxes();
    }
    return;
  }

  SendError(id, "unknown_command", "未知消息类型");
}

// ---------------------------------------------------------------------------
// 遥测推送
// ---------------------------------------------------------------------------

std::string RobotService::BuildStateJson() const {
  const RobotState s = client_.Snapshot();
  const GaitLimits limits = LimitsOf(s.gait);

  // RL 起立后运动主机仍报「坐下」。芯片若跟着撒谎，人会以为没起来而连点起立。
  const char* state_text = ToString(s.basic_state);
  if (s.rl_standing && s.basic_state == BasicState::kSitting) {
    state_text = "RL 站立 · 可走";
  }

  JsonWriter w;
  w.BeginObject()
      .Key("t", "state")
      .Key("alive", s.telemetry_alive)
      .Key("basic_state", static_cast<int>(s.basic_state))
      .Key("basic_state_text", state_text)
      .Key("rl_standing", s.rl_standing)
      .Key("gait", static_cast<int>(s.gait))
      .Key("gait_key", GaitKey(s.gait))
      .Key("gait_text", ToString(s.gait))
      .Key("mode", static_cast<int>(s.control_mode))
      .Key("height_gear", s.body_height_gear)
      .Key("mileage_cm", s.current_mileage_cm)
      .Key("emergency_source", static_cast<unsigned>(s.emergency_source));

  w.BeginObject("odom")
      .Key("x", s.odom_x)
      .Key("y", s.odom_y)
      .Key("yaw", s.odom_yaw)
      .EndObject();
  w.BeginObject("vel")
      .Key("x", s.vel_x)
      .Key("y", s.vel_y)
      .Key("yaw", s.vel_yaw)
      .EndObject();
  w.BeginObject("att")
      .Key("roll", s.roll, 1)
      .Key("pitch", s.pitch, 1)
      .Key("yaw", s.yaw, 1)
      .EndObject();
  w.BeginObject("battery")
      .Key("level", static_cast<unsigned>(s.battery_level))
      .Key("voltage", s.battery_voltage, 1)
      .EndObject();

  const float motor_max =
      *std::max_element(s.motor_temperature, s.motor_temperature + 12);
  w.BeginObject("temp")
      .Key("cpu", s.cpu_temperature, 1)
      .Key("motor_max", motor_max, 1)
      .EndObject();

  w.BeginObject("limits")
      .Key("forward", limits.max_forward_mps, 2)
      .Key("lateral", limits.max_lateral_mps, 2)
      .Key("yaw", limits.max_yaw_radps, 2)
      .EndObject();

  w.BeginArray("errors");
  const std::string errors = DescribeErrors(s.error_state);
  if (!errors.empty()) {
    size_t start = 0;
    while (start < errors.size()) {
      const size_t sep = errors.find(", ", start);
      if (sep == std::string::npos) {
        w.Value(errors.substr(start));
        break;
      }
      w.Value(errors.substr(start, sep - start));
      start = sep + 2;
    }
  }
  w.EndArray();

  w.EndObject();
  return w.Take();
}

void RobotService::StateLoop() {
  const auto period =
      std::chrono::milliseconds(1000 / std::max(1, cfg_.state_rate_hz));
  auto next = Clock::now();
  int tick = 0;
  while (running_.load()) {
    if (server_.ClientCount() > 0) {
      server_.Broadcast(BuildStateJson());

      // 点云状态 1 Hz 就够，它变化慢。单独发是因为里面要带感知主机的错误原因，
      // 塞进 10 Hz 的遥测里纯属浪费带宽。
      if (cloud_ && tick % std::max(1, cfg_.state_rate_hz) == 0) {
        server_.Broadcast(cloud_->StatusJson());
      }
    }
    ++tick;

    // 租约到期主动广播一次，让各端的"谁在控制"指示及时更新。
    {
      bool expired = false;
      {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (controller_ != 0 && Clock::now() > lease_expiry_) {
          controller_ = 0;
          expired = true;
        }
      }
      if (expired) {
        client_.ReleaseAxes();
        client_.SetCommanding(false);
        BroadcastControlState();
        std::printf("[ws] 控制权租约超时，已释放并清零轴指令\n");
      }
    }

    next += period;
    const auto now = Clock::now();
    if (next < now) next = now + period;
    std::this_thread::sleep_until(next);
  }
}

}  // namespace x30
