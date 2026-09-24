// 消防炮：连炮台 TCP Server（现场 192.168.1.253:4000），按网络版协议发 13 字节帧。
//
// 帧 = 头 08 00 00 00 08 + 8 字节位标志。持续指令约 100 ms 一次；
// 全 0 是停止。开阀才是出水，雾/柱只是花型。
// 遥控端 20 Hz 送来的摇杆量在这里合成位标志，本类自己节流。
//
// 不绑运动控制权：2.4G 档 yield 后仍走 MESH 下发。连不上不让网关起不来。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace x30 {

inline constexpr size_t kCannonFrameSize = 13;
inline constexpr uint8_t kCannonHdr[5] = {0x08, 0x00, 0x00, 0x00, 0x08};

struct CannonConfig {
  std::string host = "192.168.1.253";
  uint16_t port = 4000;
};

struct CannonCmd {
  float pan = 0;   // 右为正
  float tilt = 0;  // 上为正
  char spray[8] = {};  // fog / jet / 空
  bool fire = false;
};

struct CannonTelem {
  bool have_pose = false;
  uint8_t pan_deg = 0;
  uint8_t pan_dir = 0;  // 01 原点左，02 原点右
  uint8_t tilt_deg = 0;
  uint8_t tilt_dir = 0;  // 01 原点下，02 原点上
  uint8_t limit = 0;
  bool have_pressure = false;
  float pressure_mpa = 0;
};

void EncodeCannonFrame(const CannonCmd& cmd, uint8_t out[kCannonFrameSize]);
bool ParseCannonReply(const uint8_t* data, size_t len, CannonTelem* out);
bool CannonFrameIdle(const uint8_t frame[kCannonFrameSize]);

class CannonClient {
 public:
  explicit CannonClient(CannonConfig config);
  ~CannonClient();

  CannonClient(const CannonClient&) = delete;
  CannonClient& operator=(const CannonClient&) = delete;

  void Start();
  void Stop();

  void SetAim(float pan, float tilt);
  void SetSpray(const std::string& value);
  void SetFire(bool on);
  void Idle();

  bool online() const { return online_.load(); }
  std::string Json() const;

 private:
  void Loop();
  bool EnsureConnected();
  void CloseFd();
  bool SendFrame(const uint8_t frame[kCannonFrameSize]);
  void DrainReplies();
  CannonCmd Snapshot(bool* stale) const;

  CannonConfig cfg_;
  mutable std::mutex mutex_;
  CannonCmd cmd_{};
  CannonTelem telem_{};
  std::chrono::steady_clock::time_point last_cmd_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> online_{false};
  std::thread thread_;
  int fd_ = -1;
  bool warned_ = false;
  bool sent_idle_ = true;
  uint8_t rx_[64]{};
  size_t rx_len_ = 0;
};

}  // namespace x30
