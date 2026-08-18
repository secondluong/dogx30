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
  if (released) BroadcastControlState();
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

  JsonWriter w;
  w.BeginObject()
      .Key("t", "state")
      .Key("alive", s.telemetry_alive)
      .Key("basic_state", static_cast<int>(s.basic_state))
      .Key("basic_state_text", ToString(s.basic_state))
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
