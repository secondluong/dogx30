#include "x30/media_registry.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "x30/json.hpp"

namespace x30 {
namespace {

MediaRendition ParseRendition(const Json& j) {
  MediaRendition r;
  r.path = j.String("path");
  r.codec = j.String("codec", "h264");
  r.kbps = static_cast<int>(j.Number("kbps", 0));
  r.label = j.String("label");
  return r;
}

bool CanDecode(const MediaRendition& r, bool h264, bool h265) {
  if (r.path.empty()) return false;
  if (r.codec == "h265") return h265;
  return h264;
}

}  // namespace

bool LoadMediaConfig(const std::string& path, MediaConfig* out,
                     std::string* error) {
  std::ifstream in(path);
  if (!in) {
    if (error) *error = "打不开 " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  bool ok = false;
  const Json root = Json::Parse(ss.str(), &ok);
  if (!ok) {
    if (error) *error = path + " 不是合法 JSON";
    return false;
  }

  MediaConfig cfg;
  cfg.webrtc_base = root.String("webrtc_base", cfg.webrtc_base);
  cfg.budget_kbps = static_cast<int>(root.Number("budget_kbps", 3800));

  const Json& arr = root["sources"];
  for (size_t i = 0; i < arr.Size(); ++i) {
    const Json& s = arr.At(i);
    MediaSource src;
    src.id = s.String("id");
    src.name = s.String("name", src.id);
    src.main = ParseRendition(s["main"]);
    src.sub = ParseRendition(s["sub"]);
    src.has_audio = s.Bool("audio", false);
    src.has_ptz = s.Bool("ptz", false);
    if (src.id.empty()) {
      if (error) *error = path + " 中有源缺少 id";
      return false;
    }
    // 两路都没配等于这个源不可用，早点报出来比运行时白屏强。
    if (src.main.path.empty() && src.sub.path.empty()) {
      if (error) *error = "源 " + src.id + " 既没有 main 也没有 sub";
      return false;
    }
    cfg.sources.push_back(std::move(src));
  }

  *out = std::move(cfg);
  return true;
}

MediaRegistry::MediaRegistry(MediaConfig config) : cfg_(std::move(config)) {}

void MediaRegistry::SetCaps(ClientId id, bool h264, bool h265) {
  std::lock_guard<std::mutex> lock(mutex_);
  ClientState& cs = clients_[id];
  cs.h264 = h264;
  cs.h265 = h265;
}

const MediaSource* MediaRegistry::Find(const std::string& id) const {
  for (const auto& s : cfg_.sources) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

bool MediaRegistry::Select(ClientId id, const std::string& source_id,
                           std::string* reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  const MediaSource* src = Find(source_id);
  if (src == nullptr) {
    if (reason) *reason = "没有这个视频源";
    return false;
  }

  ClientState& cs = clients_[id];
  cs.selected = source_id;

  // 解不了主码流的客户端会退到子码流，只吃几百 kbps，用不上全码率槽位。
  // 让它占着槽位就是白白挡住真正用得上的人，所以这里主动让出。
  if (!CanDecode(src->main, cs.h264, cs.h265)) {
    if (full_holder_ == id) full_holder_ = 0;
    if (reason) {
      *reason = "遥控端不支持该源主码流的编码格式，已回退到子码流";
    }
    return false;
  }

  // 全码率槽位先到先得。抢不到不是错误，只是拿缩略图，得说清楚原因。
  if (full_holder_ == 0 || full_holder_ == id) {
    full_holder_ = id;
    return true;
  }
  if (reason) {
    *reason = "已有其他客户端在看全码率画面，本端只能拿缩略图";
  }
  return false;
}

void MediaRegistry::Forget(ClientId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  clients_.erase(id);
  if (full_holder_ == id) full_holder_ = 0;
}

MediaRegistry::ClientId MediaRegistry::full_quality_holder() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return full_holder_;
}

const MediaRendition* MediaRegistry::Pick(const MediaSource& src,
                                          const ClientState& cs,
                                          bool want_main,
                                          bool* downgraded) const {
  *downgraded = false;

  if (!want_main) {
    // 要缩略图就只能给子码流。**不允许向上回退到主码流** ——
    // 那会让一路"缩略图"按几 Mbps 下发，带宽预算当场失效。
    // 机身相机就是这种情况：它只有一路 RTSP，没有子码流，
    // 于是不选中时它就是不可看的，这是诚实的结果，不是缺陷。
    if (CanDecode(src.sub, cs.h264, cs.h265)) return &src.sub;
    return nullptr;
  }

  if (CanDecode(src.main, cs.h264, cs.h265)) return &src.main;

  // 主码流解不了就退到子码流。最常见的情形是遥控端 WebView 太老没有 H.265，
  // 而主码流是 H.265 —— 这时退到 H.264 子码流，画面糊但至少能看，
  // 并且会在计划里标出来，让操作员知道为什么糊。
  if (CanDecode(src.sub, cs.h264, cs.h265)) {
    *downgraded = true;
    return &src.sub;
  }
  return nullptr;
}

std::string MediaRegistry::BuildPlanJson(ClientId id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  ClientState cs;
  const auto it = clients_.find(id);
  if (it != clients_.end()) cs = it->second;

  const bool has_full_slot = (full_holder_ == id);

  JsonWriter w;
  w.BeginObject();
  w.Key("t", "media_plan");
  w.Key("main", cs.selected);
  w.Key("webrtc_base", cfg_.webrtc_base);
  w.Key("budget_kbps", cfg_.budget_kbps);

  int total = 0;
  w.BeginArray("sources");
  for (const auto& src : cfg_.sources) {
    const bool is_selected = src.id == cs.selected;
    const bool wants_main = has_full_slot && is_selected;
    bool downgraded = false;
    const MediaRendition* r = Pick(src, cs, wants_main, &downgraded);

    // 操作员明确选了这一路当主视图却没拿到全码率时，必须说清楚为什么。
    // 默默给一张糊图是最差的处理 —— 现场会以为是相机坏了或网络不好。
    const char* why = nullptr;
    if (is_selected && !(wants_main && !downgraded)) {
      if (!CanDecode(src.main, cs.h264, cs.h265)) {
        why = "遥控端不支持该源主码流的编码格式，已回退到子码流";
      } else if (!has_full_slot) {
        why = "已有其他客户端在看全码率画面，本端只能拿缩略图";
      }
    }

    w.BeginObject();
    w.Key("id", src.id);
    w.Key("name", src.name);
    w.Key("audio", src.has_audio);
    w.Key("ptz", src.has_ptz);

    if (r == nullptr) {
      w.Key("available", false);
      if (!wants_main && src.sub.path.empty()) {
        // 只有一路码流的源（如机身相机）。不是坏了，是没有缩略图可给，
        // 说清楚"选为主视图就能看"，否则现场会以为相机故障。
        w.Key("reason", "该源只有一路码流，选为主视图才能观看");
      } else {
        // 两路都解不了。多半是遥控端只支持 H.264 而这个源两路都是 H.265，
        // 需要转码兜底 —— 转码还没实现，先如实告诉遥控端而不是给个空路径。
        w.Key("reason", "遥控端不支持该源的编码格式，需要转码");
      }
      w.EndObject();
      continue;
    }

    w.Key("available", true);
    w.Key("path", r->path);
    w.Key("codec", r->codec);
    w.Key("kbps", r->kbps);
    w.Key("label", r->label);
    w.Key("quality", wants_main && !downgraded ? "main" : "thumb");
    if (why != nullptr) {
      w.Key("downgraded", true);
      w.Key("reason", why);
    }
    w.EndObject();
    total += r->kbps;
  }
  w.EndArray();

  // 把核算结果一并下发，超预算时遥控端可以提示操作员。理论上不该发生，
  // 发生了说明相机上配的码率和这里的预算对不上，是配置问题。
  w.Key("total_kbps", total);
  w.Key("over_budget", total > cfg_.budget_kbps);
  w.EndObject();
  return w.Take();
}

}  // namespace x30
