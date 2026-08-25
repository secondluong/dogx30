// 媒体源清单与「媒体计划」下发。
//
// 网关不搬运视频字节，只做编排：知道有哪些源、每个源有哪些码流、遥控端能解
// 什么码，然后算出一份计划告诉遥控端该拉哪一路。视频本身由 MediaMTX 直接
// 发给遥控端，不经过本进程。设计依据见 docs/media-architecture.md。
//
// 这里承载两条硬约束：
//   1. 全码流全局只有一份 —— MESH 装不下第二路，必须由服务端强制
//   2. 遥控端解不了的编码不能发给它 —— H.265 在旧 WebView 上没有软解兜底

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace x30 {

// 一路码流。同一个物理相机通常有主、子两路，参数在相机自己的界面上配好，
// 网关只是记录并转告遥控端 —— 能不转码就不转码。
struct MediaRendition {
  std::string path;    // MediaMTX 里的路径名
  std::string codec;   // "h264" / "h265"
  int kbps = 0;        // 相机上配置的码率，用于带宽核算
  std::string label;   // "1080p25" 之类，给人看的
};

struct MediaSource {
  std::string id;
  std::string name;
  MediaRendition main;
  MediaRendition sub;
  bool has_audio = false;  // 是否带拾音器，决定对讲能否用这一路
  bool has_ptz = false;
};

struct MediaConfig {
  // 遥控端访问 MediaMTX 的基地址。下发时**主机名会按客户端替换**（见
  // MediaRegistry::WebrtcBaseFor），这里的主机名只在取不到连接地址时兜底；
  // 协议和端口则一直照这里用。
  std::string webrtc_base = "http://192.168.10.2:8889";

  // 视频总码率上限。留给控制报文的余量已经扣掉，见带宽预算。
  int budget_kbps = 3800;

  std::vector<MediaSource> sources;

  // 布控球云台。没配 host 时摇杆切到球机只停车、不转云台。
  std::string ptz_host;
  uint16_t ptz_port = 80;
  std::string ptz_user = "admin";
  std::string ptz_password;
  int ptz_channel = 1;
};

// 从 JSON 配置文件加载。文件不存在时返回 false 并填 error，
// 调用方可以选择继续跑（无视频）而不是拒绝启动 —— 视频挂了不该拖垮控制。
bool LoadMediaConfig(const std::string& path, MediaConfig* out,
                     std::string* error);

class MediaRegistry {
 public:
  using ClientId = uint32_t;

  explicit MediaRegistry(MediaConfig config);

  // 遥控端上报自己能解什么。必须实测上报，不能靠 UserAgent 猜 ——
  // 同一个 Chrome 版本在不同 SoC 上结论可能不同。
  void SetCaps(ClientId id, bool h264, bool h265);

  // 选择主视图。返回是否拿到了全码率；没拿到时 reason 说明原因，
  // 要让操作员知道画面为什么是糊的，而不是默默降级。
  bool Select(ClientId id, const std::string& source_id, std::string* reason);

  void Forget(ClientId id);

  // 算出给这个客户端的计划。没调用过 Select 的客户端拿到的是全缩略图。
  //
  // client_ip 是这个客户端连到本机时用的**本端**地址。配置里的 webrtc_base 只能
  // 写一个地址，而 MESH（eth1，192.168.10.0/24）和 2.4G（机身网，192.168.1.0/24）
  // 是两个互不可达的网段，写死哪个都会让另一边拉不到流。所以下发时把主机名换成
  // client_ip —— 客户端既然从那个地址进来，就一定够得到它。留空则按配置原样下发。
  std::string BuildPlanJson(ClientId id, const std::string& client_ip = "") const;

  // 把 webrtc_base 的主机名换成 host，协议和端口沿用配置。host 为空则原样返回。
  std::string WebrtcBaseFor(const std::string& host) const;

  bool empty() const { return cfg_.sources.empty(); }

  // 当前谁占着全码率槽位，0 表示没人。用于判断是否需要给别人广播新计划。
  ClientId full_quality_holder() const;

 private:
  struct ClientState {
    bool h264 = true;   // 假定都支持 H.264，这是 WebRTC 的通用底线
    bool h265 = false;  // H.265 必须实测确认，默认按不支持处理
    std::string selected;
  };

  const MediaSource* Find(const std::string& id) const;

  // 在客户端能解的范围内挑一路。返回选中的码流与是否发生了降级。
  const MediaRendition* Pick(const MediaSource& src, const ClientState& cs,
                             bool want_main, bool* downgraded) const;

  MediaConfig cfg_;
  mutable std::mutex mutex_;
  std::map<ClientId, ClientState> clients_;
  ClientId full_holder_ = 0;
};

}  // namespace x30
