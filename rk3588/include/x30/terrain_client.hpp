// 地形图模块客户端（感知主机 192.168.1.105:43899）。
//
// 与 MotionClient 是两条独立的 UDP 通道，打到不同的主机和端口。楼梯步态必须
// 由这两条通道配合才生效：只发步态指令，运动主机会静默忽略。
//
// 三点与 MotionClient 不同：
//   - 只写。模块不回确认报文，也不需要心跳，是纯配置通道。
//   - 因为收不到确认，本类的 Set* 只表示"报文发出去了"，不代表模块接受了。
//     真正的确认要看运动主机回传的步态遥测，由上层编排。
//   - 感知主机可能不可达（未接线、未启动），此时 Start() 仍然成功，
//     发送静默失败。这样设计是为了让不上楼梯的场景不受影响。

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "x30/protocol.hpp"
#include "x30/udp_endpoint.hpp"

namespace x30 {

struct TerrainClientConfig {
  std::string perception_ip = "192.168.1.105";
  uint16_t perception_port = 43899;
};

class TerrainClient {
 public:
  explicit TerrainClient(TerrainClientConfig config);
  ~TerrainClient();

  TerrainClient(const TerrainClient&) = delete;
  TerrainClient& operator=(const TerrainClient&) = delete;

  bool Start(std::string* error);
  void Stop();

  bool SetHeightMapMode(HeightMapMode mode);
  bool SetStepZMax(StepZMax v);
  bool SetVelSource(VelSource v);
  bool SetBrakeMode(BrakeMode v);

  // 开/关感知主机上的原厂 LIO（UDP :60000，0x0BAA0001）。
  // 成功指对方回了 value=0。狗应站稳，否则文档说会失败。
  bool StartLio(bool on, std::string* error = nullptr);

  // 最近一次成功发出的地形图模式，仅用于界面显示与日志。
  // 注意它反映的是"我们发了什么"，不是"模块当前处于什么模式"。
  HeightMapMode last_mode() const;
  bool has_sent_mode() const { return has_sent_mode_.load(); }

 private:
  bool SendSimple(uint32_t code, uint32_t value);

  TerrainClientConfig cfg_;
  UdpEndpoint tx_;
  std::atomic<bool> running_{false};
  std::atomic<bool> has_sent_mode_{false};

  mutable std::mutex mutex_;
  HeightMapMode last_mode_ = HeightMapMode::kSolid;
};

}  // namespace x30
