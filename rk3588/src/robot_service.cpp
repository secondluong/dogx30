#include "x30/robot_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#include "x30/json.hpp"
#include "x30/net_util.hpp"
#include "x30/xmlrpc.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#define closesocket ::close
#endif

namespace x30 {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kVersion = "0.3.0";

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
    case Gait::kLStair: return "lstair";
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
  else if (name == "lstair") *out = Gait::kLStair;
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
      gaits_(client, terrain, GaitCoordinatorConfig{}) {
  BodyMonitorConfig monitor_cfg;
  // 官方拓扑：运动主机 .103，智能控制器（本体监控服务）.106。
  const size_t dot = cfg_.settings.robot_ip.rfind('.');
  if (dot != std::string::npos) {
    monitor_cfg.host = cfg_.settings.robot_ip.substr(0, dot + 1) + "106";
  }
  body_monitor_ = std::make_unique<BodyMonitor>(std::move(monitor_cfg));
  body_monitor_->SetHandler([this](const BodyMonitorState& s) {
    client_.ApplyBodyMonitor(
        s.alive, s.motion_state, s.gait_state, s.motor_state, s.charge_state,
        s.control_mode, s.location_state, s.on_dock_state);
  });
}

RobotService::~RobotService() { Stop(); }

bool RobotService::Start(std::string* error) {
  if (running_.load()) return true;

  // 媒体配置加载失败不阻止启动。视频是附加能力，控制才是本体 ——
  // 为了一个配错的相机地址让整个网关起不来是本末倒置。
  if (!cfg_.media_config.empty()) {
    std::string yerr;
    bool yml_wrote = false;
    if (!ApplyPtzRtspToMediamtx(MediamtxPathBeside(cfg_.media_config),
                                cfg_.settings, &yerr, &yml_wrote)) {
      std::fprintf(stderr, "MediaMTX 拉流地址没写上: %s\n", yerr.c_str());
    } else if (yml_wrote) {
      if (std::system("systemctl try-restart x30-media >/dev/null 2>&1") == 0) {
        std::printf("已按设置里的 RTSP 重启 x30-media\n");
      }
    }
    MediaConfig mc;
    std::string media_err;
    if (LoadMediaConfig(cfg_.media_config, &mc, &media_err)) {
      struct CodecBind {
        const char* id;
        const std::string& configured;
        const std::string& rtsp;
      };
      const CodecBind binds[] = {
          {"ptz_vis", cfg_.settings.ptz_vis_codec, cfg_.settings.ptz_vis_rtsp},
          {"ptz_ir", cfg_.settings.ptz_ir_codec, cfg_.settings.ptz_ir_rtsp},
      };
      for (const auto& b : binds) {
        const std::string codec = EffectivePtzCodec(b.configured, b.rtsp);
        if (codec.empty()) continue;
        for (auto& src : mc.sources) {
          if (src.id != b.id) continue;
          src.main.codec = codec;
          std::printf("视频源 %s 主码流按 %s 编排\n", b.id, codec.c_str());
        }
      }
      std::printf("媒体源 %zu 路，出口 %s\n", mc.sources.size(),
                  mc.webrtc_base.c_str());
      PtzConfig pc;
      pc.host = mc.ptz_host;
      pc.port = mc.ptz_port;
      pc.user = mc.ptz_user;
      pc.password = mc.ptz_password;
      pc.channel = mc.ptz_channel;
      const std::string rtsp = !cfg_.settings.ptz_vis_rtsp.empty()
                                   ? cfg_.settings.ptz_vis_rtsp
                                   : cfg_.settings.ptz_ir_rtsp;
      if (!rtsp.empty()) {
        std::string host, user, pass, perr;
        if (ParseRtspAuthority(rtsp, &host, &user, &pass, &perr)) {
          pc.host = host;
          if (!user.empty()) pc.user = user;
          if (!pass.empty()) pc.password = pass;
        }
      }
      if (!pc.host.empty()) {
        ptz_ = std::make_unique<PtzClient>(std::move(pc));
        ptz_->Start();
      }
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
    cloud_->SetFrameHandler([this](const PointCloudFrame& f) {
      const auto now = Clock::now();
      if (last_scan_.time_since_epoch().count() != 0 &&
          now - last_scan_ > std::chrono::seconds(5)) {
        localizer_.Reset();
      }
      last_scan_ = now;
      const RobotState s = client_.Snapshot();
      if (!localizer_.seeded()) {
        if (std::hypot(s.odom_x, s.odom_y) > 0.05f) {
          localizer_.Seed(s.odom_x, s.odom_y, s.odom_yaw);
        } else {
          localizer_.Seed(0.0f, 0.0f, s.yaw * 0.0174532925f);
        }
      }
      localizer_.SetImuYaw(s.yaw * 0.0174532925f);
      if (!f.xyz.empty()) {
        localizer_.Feed(f.xyz.data(), f.xyz.size() / 3);
      }
    });
    std::printf("点云桥接已就绪：master %s，话题 %s / %s\n",
                cfg_.cloud.master_uri.c_str(), cfg_.cloud.topic.c_str(),
                cfg_.cloud.registered_topic.c_str());
  }

  server_.SetStaticRoot(cfg_.static_root);
  server_.SetHandlers([this](WsServer::ClientId id) { OnConnect(id); },
                      [this](WsServer::ClientId id, const std::string& text) {
                        OnMessage(id, text);
                      },
                      [this](WsServer::ClientId id) { OnDisconnect(id); });

  if (!server_.Start(cfg_.bind_address, cfg_.port, error)) return false;

  gaits_.Start();
  body_monitor_->Start();
  if (cloud_) cloud_->Start();
  StartBatteryRos();
  running_.store(true);
  state_thread_ = std::thread(&RobotService::StateLoop, this);
  return true;
}

void RobotService::Stop() {
  if (!running_.exchange(false)) return;
  if (state_thread_.joinable()) state_thread_.join();
  if (ptz_) ptz_->Stop();
  StopBatteryRos();
  if (body_monitor_) body_monitor_->Stop();
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
    if (ptz_) ptz_->StopMove();
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
  std::string given = msg.String("password");
  if (given.empty()) given = msg.String("token");
  if (!TokenMatches(kAdminPassword, given)) {
    std::printf("[配置] 客户端 %llu 设置密码不符，已拒绝\n",
                static_cast<unsigned long long>(id));
    SendError(id, "bad_admin_token", "密码不正确");
    return false;
  }
  return true;
}

void RobotService::HandleConfigGet(WsServer::ClientId id) {
  GatewaySettings shown = cfg_.settings;
  const std::string yml = MediamtxPathBeside(cfg_.media_config);
  if (shown.ptz_vis_rtsp.empty()) {
    shown.ptz_vis_rtsp = ReadMediamtxSource(yml, "ptz_vis_main");
  }
  if (shown.ptz_ir_rtsp.empty()) {
    shown.ptz_ir_rtsp = ReadMediamtxSource(yml, "ptz_ir_main");
  }
  if (shown.ptz_vis_codec.empty()) {
    shown.ptz_vis_codec = EffectivePtzCodec("", shown.ptz_vis_rtsp);
  }
  if (shown.ptz_ir_codec.empty()) {
    shown.ptz_ir_codec = EffectivePtzCodec("", shown.ptz_ir_rtsp);
  }
  JsonWriter w;
  w.BeginObject()
      .Key("t", "config")
      .Raw("settings", GatewaySettingsJson(shown))
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

  const std::string yml = MediamtxPathBeside(cfg_.media_config);
  bool yml_wrote = false;
  if (!ApplyPtzRtspToMediamtx(yml, next, &error, &yml_wrote)) {
    std::fprintf(stderr, "[配置] MediaMTX 拉流地址没写上：%s\n", error.c_str());
  } else if (yml_wrote) {
    if (std::system("systemctl try-restart x30-media >/dev/null 2>&1") == 0) {
      std::printf("[配置] 已按新 RTSP 重启 x30-media\n");
    }
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
  bool expired = false;
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (controller_ != 0 && Clock::now() > lease_expiry_) {
      controller_ = 0;
      lease_expiry_ = Clock::time_point{};
      expired = true;
    }
    holder = controller_;
  }
  if (expired) {
    client_.ReleaseAxes();
    client_.SetCommanding(false);
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
      // 认不认 claim.standing。旧网关没有这个键，App 据此判断该不该提示「请重装网关」，
      // 不再靠「交接 1.5 秒对不上」去猜 —— 那条路误报很多（claim 还在路上、权还没到手）。
      .Key("pose_adopt", true)
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
    // 遥控端可以在申请时把它知道的姿态一并告知：2.4G 直连下起立/趴下不经过本机，
    // 而运动主机 RL 起立后仍报坐下，本机无从得知。只在真带了这个键时采纳，
    // 而且必须拿到控制权 —— 旁观者不该改本机的记忆。
    if (granted && msg.Has("standing")) {
      client_.AdoptPosture(msg["standing"].AsBool(false));
    }
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

  // 改配置要设置密码，控制权在这里不作数：操控狗和改网关指向是两回事，
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
      gaits_.ClearQueued();
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
      gaits_.ClearQueued();
      client_.StandOrSit();
    } else if (name == "stand_up") {
      const MotionView view = client_.View();
      if (std::strcmp(view.posture, "prone") != 0) {
        SendError(id, "invalid_state", "只有趴下状态可以起立");
        return;
      }
      gaits_.ClearQueued();
      client_.StandUp();
    } else if (name == "sit_down" || name == "sit") {
      const MotionView view = client_.View();
      if (std::strcmp(view.posture, "standing") != 0) {
        SendError(id, "invalid_state", "只有站立状态可以趴下");
        return;
      }
      gaits_.ClearQueued();
      client_.SitDown();
    } else if (name == "unload") {
      const MotionView view = client_.View();
      if (std::strcmp(view.posture, "locked") != 0) {
        SendError(id, "invalid_state", "只有关节锁定状态需要卸力");
        return;
      }
      gaits_.ClearQueued();
      client_.UnloadForce();
    } else if (name == "torque") {
      const MotionView view = client_.View();
      if (std::strcmp(view.posture, "standing") != 0) {
        SendError(id, "invalid_state", "趴下、起趴过渡或锁定状态不能进入力控");
        return;
      }
      client_.EnterTorqueStand();
    } else if (name == "step") {
      if (WalkHold()) {
        SendError(id, "lio_wait", "LIO 还在对准，请站稳，不要走");
        return;
      }
      const std::string v = msg.String("value");
      const MotionView view = client_.View();
      if (v == "off" || v == "stop") {
        client_.StopStepping();
      } else if (v == "on" || v == "start") {
        if (view.phase != MotionPhase::kStopped &&
            view.phase != MotionPhase::kTorque) {
          SendError(id, "invalid_state", "只有力控或停步状态可以起步");
          return;
        }
        client_.StartStepping();
        gaits_.ApplyQueuedWhenWalking();
      } else {
        SendError(id, "bad_request", "起步/停步必须明确指定 on 或 off");
        return;
      }
    } else if (name == "savedata") {
      client_.SaveData();
    } else if (name == "gait") {
      Gait gait = Gait::kWalk;
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
          gait, style,
          [this, id, gait](const GaitCoordinator::Result& r) {
            SendGaitResult(id, gait, r);
          },
          msg.Bool("stepping") || client_.UserStepping());
      if (!accepted) {
        SendError(id, "gait_busy", "上一次步态切换尚未完成");
        return;
      }
      const MotionView view = client_.View();
      if (view.phase != MotionPhase::kWalking) {
        client_.ExpectGaitBeforeHeight(gait);
      }
    } else if (name == "height") {
      const MotionView view = client_.View();
      if (view.phase == MotionPhase::kUnavailable) {
        SendError(id, "invalid_state", "只有力控、停步或行走状态可以设置身高");
        return;
      }
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

  if (t == "ptz") {
    if (!HoldsControl(id)) {
      SendError(id, "no_control", "未持有控制权，请先发送 claim");
      return;
    }
    if (ptz_) {
      ptz_->Set(Clamp01(msg.Number("pan")), Clamp01(msg.Number("tilt")),
                Clamp01(msg.Number("zoom")));
    } else {
      // 遥控端 20 Hz 在发，这里只说一次，避免横幅刷屏。
      static bool told = false;
      const float pan = Clamp01(msg.Number("pan"));
      const float tilt = Clamp01(msg.Number("tilt"));
      const float zoom = Clamp01(msg.Number("zoom"));
      if (!told && (std::fabs(pan) > 0.08f || std::fabs(tilt) > 0.08f ||
                    std::fabs(zoom) > 0.08f)) {
        told = true;
        SendError(id, "ptz_unconfigured",
                  "布控球云台未配置，检查设置里的白光 RTSP");
      }
    }
    return;
  }

  if (t == "vel" || t == "pose" || t == "release") {
    if (!HoldsControl(id)) {
      SendError(id, "no_control", "未持有控制权，请先发送 claim");
      return;
    }
    if (t == "vel") {
      if (WalkHold()) {
        client_.SetVelocity(0, 0, 0);
        return;
      }
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
// 电池 / 里程计：运动 UDP 经常没有 0x21050F0A，RL 起立后腿式里程计
// 也整段是 0。感知主机的 /battery/*、/leg_odom、/mileage/* 作后备。
// ---------------------------------------------------------------------------

namespace {

std::string LookupRosType(const std::string& master_uri, const std::string& topic) {
  XmlRpcValue reply;
  std::string err;
  if (!XmlRpcCall(master_uri, "getTopicTypes", {XmlRpcValue::Str("/x30_batt")},
                  &reply, &err, 2000)) {
    return "";
  }
  if (reply.At(0).AsInt() != 1) return "";
  const XmlRpcValue& list = reply.At(2);
  for (size_t i = 0; i < list.Size(); ++i) {
    if (list.At(i).At(0).AsString() == topic) return list.At(i).At(1).AsString();
  }
  return "";
}

bool DecodeRosFloat(const uint8_t* data, size_t len, float* out) {
  if (len >= 8) {
    double d = 0;
    std::memcpy(&d, data, sizeof(d));
    if (d > -1e6 && d < 1e6) {
      *out = static_cast<float>(d);
      return true;
    }
  }
  if (len >= 4) {
    std::memcpy(out, data, sizeof(float));
    return true;
  }
  return false;
}

// UDP 0x0BAA0001 只是敲门。真正让 /lio_odom 出数的是 /lio_enable。
bool CallLioEnable(const std::string& master_uri, bool on) {
  XmlRpcValue reply;
  std::string err;
  if (!XmlRpcCall(master_uri, "lookupService",
                  {XmlRpcValue::Str("/x30_lio"), XmlRpcValue::Str("/lio_enable")},
                  &reply, &err, 2000)) {
    return false;
  }
  if (reply.At(0).AsInt() != 1) return false;
  const std::string uri = reply.At(2).AsString();
  const auto pos = uri.rfind(':');
  if (pos == std::string::npos) return false;
  uint16_t port = static_cast<uint16_t>(std::atoi(uri.c_str() + pos + 1));
  std::string host = uri;
  const auto slash = host.find("://");
  if (slash != std::string::npos) host = host.substr(slash + 3);
  const auto colon = host.rfind(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  if (host == "host" || host == "localhost") host = "192.168.1.105";
  const auto master_host_end = master_uri.find("://");
  if ((host == "host" || host.empty()) && master_host_end != std::string::npos) {
    host = "192.168.1.105";
  }

  const int fd = TcpConnectTimeout(host, port, 1500, 1500);
  if (fd < 0) return false;

  auto put_u32 = [](std::string* o, uint32_t v) {
    o->push_back(static_cast<char>(v & 0xFF));
    o->push_back(static_cast<char>((v >> 8) & 0xFF));
    o->push_back(static_cast<char>((v >> 16) & 0xFF));
    o->push_back(static_cast<char>((v >> 24) & 0xFF));
  };
  std::string fields;
  auto add = [&](const std::string& kv) {
    put_u32(&fields, static_cast<uint32_t>(kv.size()));
    fields += kv;
  };
  add("callerid=/x30_lio");
  add("service=/lio_enable");
  add("type=std_srvs/SetBool");
  add("md5sum=09fb03525b03e7ea1fd3992bafd87e16");
  add("persistent=0");
  std::string header;
  put_u32(&header, static_cast<uint32_t>(fields.size()));
  header += fields;
  if (::send(fd, header.data(), header.size(), 0) !=
      static_cast<ssize_t>(header.size())) {
    closesocket(fd);
    return false;
  }
  uint8_t lenb[4];
  if (::recv(fd, lenb, 4, MSG_WAITALL) != 4) {
    closesocket(fd);
    return false;
  }
  uint32_t hlen = static_cast<uint32_t>(lenb[0]) |
                  (static_cast<uint32_t>(lenb[1]) << 8) |
                  (static_cast<uint32_t>(lenb[2]) << 16) |
                  (static_cast<uint32_t>(lenb[3]) << 24);
  if (hlen > 4096) {
    closesocket(fd);
    return false;
  }
  std::vector<uint8_t> hb(hlen);
  if (hlen > 0 &&
      ::recv(fd, hb.data(), hlen, MSG_WAITALL) != static_cast<ssize_t>(hlen)) {
    closesocket(fd);
    return false;
  }
  const uint8_t req[5] = {1, 0, 0, 0, static_cast<uint8_t>(on ? 1 : 0)};
  if (::send(fd, req, 5, 0) != 5) {
    closesocket(fd);
    return false;
  }
  uint8_t ok = 0;
  if (::recv(fd, &ok, 1, MSG_WAITALL) != 1) {
    closesocket(fd);
    return false;
  }
  closesocket(fd);
  return ok == 1;
}

bool DecodeRosInt32(const uint8_t* data, size_t len, int32_t* out) {
  if (len < 4) return false;
  std::memcpy(out, data, sizeof(int32_t));
  return true;
}

// nav_msgs/Odometry：跳过 Header / child_frame_id 两个变长字符串，
// 再读 pose.position、orientation、跳过 36 元协方差，再读 twist。
bool DecodeRosOdometry(const uint8_t* data, size_t len, float* x, float* y,
                       float* yaw, float* vx, float* vy, float* wz) {
  if (len < 4 + 8 + 4) return false;
  size_t o = 4 + 8;
  uint32_t flen = 0;
  std::memcpy(&flen, data + o, 4);
  o += 4;
  if (flen > 256 || o + flen + 4 > len) return false;
  o += flen;
  uint32_t clen = 0;
  std::memcpy(&clen, data + o, 4);
  o += 4;
  if (clen > 256 || o + clen + 7 * 8 + 36 * 8 + 6 * 8 > len) return false;
  o += clen;
  double px = 0, py = 0, qx = 0, qy = 0, qz = 0, qw = 0;
  std::memcpy(&px, data + o, 8);
  std::memcpy(&py, data + o + 8, 8);
  o += 24;
  std::memcpy(&qx, data + o, 8);
  std::memcpy(&qy, data + o + 8, 8);
  std::memcpy(&qz, data + o + 16, 8);
  std::memcpy(&qw, data + o + 24, 8);
  o += 32 + 36 * 8;
  double lx = 0, ly = 0, az = 0;
  std::memcpy(&lx, data + o, 8);
  std::memcpy(&ly, data + o + 8, 8);
  std::memcpy(&az, data + o + 40, 8);
  *x = static_cast<float>(px);
  *y = static_cast<float>(py);
  *yaw = static_cast<float>(
      std::atan2(2.0 * (qw * qz + qx * qy), 1.0 - 2.0 * (qy * qy + qz * qz)));
  *vx = static_cast<float>(lx);
  *vy = static_cast<float>(ly);
  *wz = static_cast<float>(az);
  return true;
}

bool DecodeRosImuRpy(const uint8_t* data, size_t len, float* roll, float* pitch,
                     float* yaw) {
  if (len < 4 + 8 + 4) return false;
  size_t o = 4 + 8;
  uint32_t flen = 0;
  std::memcpy(&flen, data + o, 4);
  o += 4;
  if (flen > 256 || o + flen + 4 * 8 > len) return false;
  o += flen;
  double qx = 0, qy = 0, qz = 0, qw = 0;
  std::memcpy(&qx, data + o, 8);
  std::memcpy(&qy, data + o + 8, 8);
  std::memcpy(&qz, data + o + 16, 8);
  std::memcpy(&qw, data + o + 24, 8);
  const double rad2deg = 57.2957795;
  *roll = static_cast<float>(
      std::atan2(2.0 * (qw * qx + qy * qz), 1.0 - 2.0 * (qx * qx + qy * qy)) *
      rad2deg);
  double sinp = 2.0 * (qw * qy - qz * qx);
  if (sinp > 1.0) sinp = 1.0;
  if (sinp < -1.0) sinp = -1.0;
  *pitch = static_cast<float>(std::asin(sinp) * rad2deg);
  *yaw = static_cast<float>(
      std::atan2(2.0 * (qw * qz + qx * qy), 1.0 - 2.0 * (qy * qy + qz * qz)) *
      rad2deg);
  return true;
}

bool DecodeRosLevel(const uint8_t* data, size_t len, const std::string& type,
                    uint8_t* out) {
  if (type.find("UInt8") != std::string::npos) {
    if (len < 1) return false;
    *out = data[0] > 100 ? 100 : data[0];
    return true;
  }
  float v = 0;
  if (!DecodeRosFloat(data, len, &v)) return false;
  if (v <= 1.01f && v >= 0.0f) {
    *out = static_cast<uint8_t>(v * 100.0f + 0.5f);
  } else if (v > 1.01f && v <= 100.0f) {
    *out = static_cast<uint8_t>(v + 0.5f);
  } else {
    return false;
  }
  return true;
}

}  // namespace

void RobotService::StartBatteryRos() {
  if (battery_ros_ || cfg_.cloud.master_uri.empty() ||
      cfg_.cloud.node_host.empty()) {
    return;
  }

  // 本机实测：level 是 UInt8，voltage 是 Float32。查不到类型时按这个兜底，
  // 再用 Float32 去订 level 会被发布者因 md5 直接拒掉。
  std::string level_type = LookupRosType(cfg_.cloud.master_uri, "/battery/level");
  std::string volt_type = LookupRosType(cfg_.cloud.master_uri, "/battery/voltage");
  if (level_type.empty()) level_type = "std_msgs/UInt8";
  if (volt_type.empty()) volt_type = "std_msgs/Float32";

  RosClientConfig rc;
  rc.master_uri = cfg_.cloud.master_uri;
  rc.node_host = cfg_.cloud.node_host;
  rc.node_name = "/x30_batt";
  rc.retry_ms = 4000;

  battery_ros_.reset(new RosClient(rc));
  // md5=*：云深处 UInt8 的校验和与官方差一位，对不上会被直接踢掉。
  battery_ros_->Subscribe({"/battery/level", level_type, "*"},
                          [this, level_type](const uint8_t* d, size_t n) {
                            uint8_t level = 0;
                            if (!DecodeRosLevel(d, n, level_type, &level)) return;
                            const RobotState s = client_.Snapshot();
                            client_.ApplyBattery(level, s.battery_voltage, false);
                          });
  battery_ros_->Subscribe({"/battery/voltage", volt_type, "*"},
                          [this](const uint8_t* d, size_t n) {
                            float v = 0;
                            if (!DecodeRosFloat(d, n, &v)) return;
                            if (v > 200.0f) v = v / 1000.0f;
                            const RobotState s = client_.Snapshot();
                            client_.ApplyBattery(s.battery_level, v, false);
                          });
  battery_ros_->Subscribe(
      {"/leg_odom", "nav_msgs/Odometry", "*"},
      [this](const uint8_t* d, size_t n) {
        float x = 0, y = 0, yaw = 0, vx = 0, vy = 0, wz = 0;
        if (!DecodeRosOdometry(d, n, &x, &y, &yaw, &vx, &vy, &wz)) return;
        client_.ApplyOdom(x, y, yaw, vx, vy, wz, false);
      });
  battery_ros_->Subscribe(
      {"/lio_odom", "nav_msgs/Odometry", "*"},
      [this](const uint8_t* d, size_t n) {
        float x = 0, y = 0, yaw = 0, vx = 0, vy = 0, wz = 0;
        if (!DecodeRosOdometry(d, n, &x, &y, &yaw, &vx, &vy, &wz)) return;
        NoteLioSample(x, y, yaw);
      });
  battery_ros_->Subscribe(
      {"/imu_from_motion", "sensor_msgs/Imu", "*"},
      [this](const uint8_t* d, size_t n) {
        float roll = 0, pitch = 0, yaw = 0;
        if (!DecodeRosImuRpy(d, n, &roll, &pitch, &yaw)) return;
        client_.ApplyAtt(roll, pitch, yaw, false);
      });
  battery_ros_->Subscribe(
      {"/mileage/current_mileage", "std_msgs/Int32", "*"},
      [this](const uint8_t* d, size_t n) {
        int32_t cm = 0;
        if (!DecodeRosInt32(d, n, &cm)) return;
        client_.ApplyMileage(cm, false);
      });

  std::string err;
  if (!battery_ros_->Start(&err)) {
    std::fprintf(stderr, "[电池] ROS 订阅未启动：%s\n", err.c_str());
    battery_ros_.reset();
    return;
  }
  std::printf("[遥测] 已订阅 %s /battery/* /leg_odom /lio_odom /imu /mileage\n",
              cfg_.cloud.master_uri.c_str());
}

void RobotService::StopBatteryRos() {
  if (!battery_ros_) return;
  battery_ros_->Stop();
  battery_ros_.reset();
}

void RobotService::NoteLioSample(float x, float y, float yaw) {
  const auto now = Clock::now();
  {
    std::lock_guard<std::mutex> lock(lio_mutex_);
    lio_valid_ = true;
    lio_x_ = x;
    lio_y_ = y;
    lio_yaw_ = yaw;
    lio_last_msg_ = now;
    if (!lio_got_msg_) {
      lio_got_msg_ = true;
      lio_first_msg_ = now;
      lio_still_since_ = now;
      lio_have_prev_ = false;
    }
    if (lio_have_prev_) {
      const float dx = std::hypot(x - lio_prev_x_, y - lio_prev_y_);
      float dyaw = std::fabs(yaw - lio_prev_yaw_);
      if (dyaw > 3.14159265f) dyaw = 6.2831853f - dyaw;
      if (dx > 0.035f || dyaw > 0.06f) {
        lio_still_since_ = now;
        if (lio_ready_ && dx > 0.25f) lio_ready_ = false;
      }
    }
    lio_prev_x_ = x;
    lio_prev_y_ = y;
    lio_prev_yaw_ = yaw;
    lio_have_prev_ = true;
    if (!lio_ready_ && now - lio_first_msg_ > std::chrono::milliseconds(2000) &&
        now - lio_still_since_ > std::chrono::milliseconds(1500)) {
      lio_ready_ = true;
      std::printf("[LIO] 已对准，可以走\n");
    }
  }
  if (cloud_) cloud_->SetWorldPose(x, y);
}

bool RobotService::WalkHold() const {
  if (!cloud_) return false;
  const RobotState s = client_.Snapshot();
  // 拦走路只看运动主机自己报的站立。RL 记忆 + 遥测坐下时狗经常其实趴着，
  // 这时还按「对准中」会把 MESH 摇杆和起步全锁死。
  if (!TelemUpright(s.basic_state)) return false;
  std::lock_guard<std::mutex> lock(lio_mutex_);
  if (!lio_ready_) return true;
  return !lio_got_msg_ ||
         Clock::now() - lio_last_msg_ > std::chrono::milliseconds(800);
}

// ---------------------------------------------------------------------------
// 遥测推送
// ---------------------------------------------------------------------------

std::string RobotService::BuildStateJson() const {
  const RobotState s = client_.Snapshot();
  const MotionView view = client_.View();
  const GaitLimits limits = LimitsOf(s.gait);

  bool lio_ready = false, lio_got = false, lio_fresh = false;
  {
    std::lock_guard<std::mutex> lock(lio_mutex_);
    lio_got = lio_got_msg_;
    lio_ready = lio_ready_;
    lio_fresh = lio_got &&
                Clock::now() - lio_last_msg_ < std::chrono::milliseconds(800);
  }
  const bool aligning =
      cloud_ && TelemUpright(s.basic_state) && !(lio_ready && lio_fresh);

  // RL 起立后运动主机仍报「坐下」。芯片若跟着撒谎，人会以为没起来而连点起立。
  const char* state_text = ToString(s.basic_state);
  if (JointsLocked(s.basic_state, s.emergency_source)) {
    state_text = "急停锁定";
  } else if (s.body_monitor_alive &&
             (!s.telemetry_alive || s.body_motion_state == 6 ||
              s.body_motion_state == 7 || s.body_motion_state == 16)) {
    switch (s.body_motion_state) {
      case 0: state_text = "趴下"; break;
      case 1: state_text = "起立中"; break;
      case 2: state_text = "站立"; break;
      case 3: state_text = "力控"; break;
      case 4: state_text = "行走"; break;
      case 5: state_text = "趴下中"; break;
      case 6: state_text = "软急停"; break;
      case 7: state_text = "摔倒"; break;
      case 16: state_text = "RL"; break;
      default: state_text = "本体状态未知"; break;
    }
  } else if (s.rl_standing && s.basic_state == BasicState::kSitting) {
    state_text = aligning ? "RL 站立 · 对准中" : "RL 站立 · 可走";
  }

  JsonWriter w;
  w.BeginObject()
      .Key("t", "state")
      .Key("alive", s.telemetry_alive)
      // 规范化状态由当前控制层生成。网页只消费这组字段，不再按按钮历史猜状态。
      .Key("state_source", "mesh")
      .Key("state_truth", s.telemetry_alive
                              ? "motion_udp"
                              : (s.body_monitor_alive ? "body_monitor"
                                                      : "unavailable"))
      .Key("state_valid", view.state_valid)
      .Key("posture", view.posture)
      .Key("motion", view.motion)
      .Key("axis_mode", view.axis_mode)
      .Key("basic_state", static_cast<int>(s.basic_state))
      .Key("basic_state_text", state_text)
      .Key("rl_standing", s.rl_standing)
      .Key("body_monitor_alive", s.body_monitor_alive)
      .Key("body_motion_state", s.body_motion_state)
      .Key("body_gait_state", s.body_gait_state)
      .Key("body_motor_state", s.body_motor_state)
      .Key("body_charge_state", s.body_charge_state)
      .Key("body_control_mode", s.body_control_mode)
      .Key("body_location_state", s.body_location_state)
      .Key("body_on_dock_state", s.body_on_dock_state)
      .Key("gait", static_cast<int>(s.gait))
      .Key("gait_key", GaitKey(s.gait))
      .Key("gait_text", ToString(s.gait))
      .Key("mode", static_cast<int>(s.control_mode))
      .Key("height_gear", s.body_height_gear)
      .Key("mileage_cm", s.current_mileage_cm)
      .Key("emergency_source", static_cast<unsigned>(s.emergency_source));

  float ox = s.odom_x, oy = s.odom_y, oyaw = s.odom_yaw;
  const char* odom_source = "leg";
  bool have_lio = false;
  {
    std::lock_guard<std::mutex> lock(lio_mutex_);
    have_lio = lio_valid_;
    if (have_lio) {
      ox = lio_x_;
      oy = lio_y_;
      oyaw = lio_yaw_;
      odom_source = "lio";
    }
  }
  if (!have_lio) {
    const LocalizerPose lp = localizer_.pose();
    if (lp.valid) {
      ox = lp.x;
      oy = lp.y;
      oyaw = lp.yaw;
      odom_source = "scan";
    }
  }

  w.BeginObject("odom")
      .Key("x", ox)
      .Key("y", oy)
      .Key("yaw", oyaw)
      .EndObject();
  w.Key("odom_source", odom_source);
  {
    const char* lio_text = "";
    if (aligning) {
      lio_text = lio_got ? "LIO 还不稳，请继续站稳，不要走"
                         : "LIO 正在对准，请站稳，不要走";
    }
    w.BeginObject("lio")
        .Key("ready", lio_ready && lio_fresh)
        .Key("aligning", aligning)
        .Key("text", lio_text)
        .EndObject();
  }
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
      .Key("valid", s.battery_valid)
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

    // 原厂 LIO：UDP 敲门 + /lio_enable。文档要求站稳再开，坐下时不发。
    if (cloud_ && !lio_valid_ && tick % 40 == 20) {
      const RobotState st = client_.Snapshot();
      if (StandingForLio(st.basic_state, st.rl_standing, st.emergency_source)) {
        std::string err;
        if (!lio_cmd_ok_ && terrain_.StartLio(true, &err)) {
          lio_cmd_ok_ = true;
        }
        if (CallLioEnable(cfg_.cloud.master_uri, true)) {
          lio_cmd_ok_ = true;
        }
      }
    }

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
