// 气体监测主板 UDP 接收。
//
// 主板主动上报，约每 2 秒一帧，不需要查询，无应答。串口服务器以 UDP
// Client 把二进制帧打到本机固定端口（默认 1000）。
//
// 帧格式（65 字节，`3A 02`）：
//   3A 02 00 | [槽位×10] | CRC_H CRC_L
// 每槽 6 字节：端口号、气体类型 ID、IEEE754 大端 float 浓度。
// 同一拍还可能带 0x0B 电量帧，本类只认气体。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "x30/payload_switch.hpp"
#include "x30/udp_endpoint.hpp"

namespace x30 {

inline constexpr int kGasSlotCount = 10;
inline constexpr size_t kGasFrameSize = 65;
inline constexpr uint16_t kGasDefaultPort = 1000;

// UWB 标签 3A 56，与气体同一条 UDP。帧里的 ID 原样收下，不再按名单过滤。
// 同时记住的标签有上限，只为挡住垃圾帧把状态撑满。
inline constexpr size_t kUwbFrameSize = 23;
inline constexpr int kUwbTagCap = 16;

enum class GasSlotStatus {
  kEmpty,    // ID=0 且浓度为 0：空槽
  kOk,       // 有读数
  kOffline,  // 浓度 0xFFFFFFFF：该路通讯中断 / 探头掉线
};

struct GasSlot {
  uint8_t port = 0;      // 1–10
  uint8_t type_id = 0;   // 0–10，见协议
  float value = 0.0f;
  GasSlotStatus status = GasSlotStatus::kEmpty;
};

struct GasSnapshot {
  bool alive = false;  // 最近一帧还在超时窗口内
  bool heard = false;  // 曾经成功解析过至少一帧
  GasSlot slots[kGasSlotCount];
};

struct UwbTag {
  uint8_t id = 0;
  uint8_t subtype = 0;
  bool valid = false;
  bool heard = false;
  float x = 0.0f;  // 米，相对狗 / UWB 原点
  float y = 0.0f;
  float z = 0.0f;
  uint32_t tick_ms = 0;
};

struct UwbSnapshot {
  bool alive = false;
  bool heard = false;
  std::vector<UwbTag> tags;
};

struct GasClientConfig {
  uint16_t local_port = kGasDefaultPort;
  // 约 2 秒一帧。6 秒没来视为掉线，对应三帧都丢。
  int timeout_ms = 6000;
  // UWB 约 1 秒一帧。4 秒没来视为该标签无效。
  int uwb_timeout_ms = 4000;
};

const char* GasTypeKey(uint8_t id);
const char* GasTypeName(uint8_t id);
const char* GasTypeUnit(uint8_t id);
const char* GasStatusText(GasSlotStatus s);
// 端口 1–5 板1，6–10 板2。对不上返回 0。
int GasBoardOfPort(uint8_t port);

// CRC-16/MODBUS，覆盖帧头到槽位末（前 63 字节）。算法未在协议里写死，
// 解析不依赖它通过；对得上只作为额外校验。
uint16_t GasCrc16(const uint8_t* data, size_t len);

// 从一段 UDP 载荷里找出 `3A 02 00` 气体帧。载荷里可能还拼着电量帧。
// 找到并解析成功返回 true；slots 按端口 1–10 写入（缺的槽保持原值）。
bool ParseGasFrame(const uint8_t* data, size_t len, GasSlot slots[kGasSlotCount],
                   bool* crc_ok = nullptr);

// 组一帧，给测试和仿真器用。crc 按 CRC-16/MODBUS 填写。
void BuildGasFrame(const GasSlot slots[kGasSlotCount],
                   uint8_t out[kGasFrameSize]);

// 从一段 UDP 载荷里找出 `3A 56`。任何标签 ID 都收。valid=0 时仍返回 true，但 xyz 应丢弃。
bool ParseUwbFrame(const uint8_t* data, size_t len, UwbTag* out,
                   bool* crc_ok = nullptr);
void BuildUwbFrame(uint8_t subtype, uint8_t id, bool valid, int32_t x_mm,
                   int32_t y_mm, int32_t z_mm, uint32_t tick_ms,
                   uint8_t out[kUwbFrameSize]);

class GasClient {
 public:
  explicit GasClient(GasClientConfig config);
  ~GasClient();

  GasClient(const GasClient&) = delete;
  GasClient& operator=(const GasClient&) = delete;

  bool Start(std::string* error);
  void Stop();

  GasSnapshot Snapshot() const;
  UwbSnapshot SnapshotUwb() const;
  // 一段完整的 JSON 对象，给 state.gas 直接嵌进去。
  std::string Json() const;
  std::string UwbJson() const;

  // 载荷开关应答跟气体走同一条 UDP Client。发命令必须从 :1000 回源。
  bool SendTo(const std::string& ip, uint16_t port, const void* data,
              size_t len);
  void SetSwitchAckHandler(std::function<void(const SwitchAck&)> fn) {
    on_switch_ack_ = std::move(fn);
  }
  void SetPayloadPeerHandler(
      std::function<void(const std::string&, uint16_t)> fn) {
    on_peer_ = std::move(fn);
  }

 private:
  void RxLoop();
  void ApplyFrame(const GasSlot slots[kGasSlotCount]);
  void ApplyUwb(const UwbTag& tag);

  GasClientConfig cfg_;
  UdpEndpoint rx_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;
  GasSnapshot snap_{};
  std::chrono::steady_clock::time_point last_frame_{};
  bool logged_first_ = false;
  struct UwbTrack {
    UwbTag tag;
    std::chrono::steady_clock::time_point last{};
  };
  std::vector<UwbTrack> uwb_tracks_;
  bool uwb_heard_ = false;
  bool logged_uwb_ = false;
  std::function<void(const SwitchAck&)> on_switch_ack_;
  std::function<void(const std::string&, uint16_t)> on_peer_;
};

}  // namespace x30
