// 载荷开关：条纹灯 / 气泵 / UWB / 风扇 / 摄像头。
//
// 只实现协议里已经写死的那几路。灯板走自组网 UDP :9000；其余四路走气体
// 同一条串口服务器（应答会打回本机 :1000，不回源端口）。
//
// 帧是协议给的固定字节，不在这里重算 CRC。应答 11 字节：
//   AA 55 00 0B | cmd | err | state | CRC_H CRC_L | 0D 0A
// err=0 成功，1 失败；state=1 开、0 关。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "x30/udp_endpoint.hpp"

namespace x30 {

enum class SwitchStatus {
  kUnknown,
  kOn,
  kOff,
  kBusy,
  kError,
};

struct SwitchItem {
  const char* key;
  const char* name;
  uint8_t cmd;
  bool via_light;  // true = 灯板 :9000；false = 载荷主板
  SwitchStatus status = SwitchStatus::kUnknown;
};

struct SwitchAck {
  uint8_t cmd = 0;
  bool ok = false;
  bool on = false;
};

struct PayloadSwitchConfig {
  std::string payload_ip = "192.168.1.201";
  uint16_t payload_port = 2000;
  std::string light_ip = "192.168.1.200";
  uint16_t light_port = 9000;
  int ack_timeout_ms = 800;
};

const char* SwitchStatusText(SwitchStatus s);
bool ParseSwitchAck(const uint8_t* data, size_t len, SwitchAck* out);

// 发给气体口的载荷命令。GasClient 绑着 :1000，应答也从这条回来。
using PayloadSendFn =
    std::function<bool(const std::string& ip, uint16_t port, const void* data,
                       size_t len)>;

class PayloadSwitch {
 public:
  using ResultFn = std::function<void(const std::string& key, bool ok, bool on,
                                      const char* status, const char* msg)>;

  explicit PayloadSwitch(PayloadSwitchConfig config);
  ~PayloadSwitch();

  PayloadSwitch(const PayloadSwitch&) = delete;
  PayloadSwitch& operator=(const PayloadSwitch&) = delete;

  void SetPayloadSender(PayloadSendFn send) { payload_send_ = std::move(send); }
  void SetResultHandler(ResultFn fn) { on_result_ = std::move(fn); }

  bool Start(std::string* error);
  void Stop();

  // 气体口学到的对端（串口服务器）。有的话优先于配置里的 payload_ip。
  void NotePayloadPeer(const std::string& ip, uint16_t port);
  void OnPayloadAck(const SwitchAck& ack);

  // 异步。busy 时拒绝并立刻回调。
  bool Request(const std::string& key, bool on, std::string* error);

  std::string Json() const;

 private:
  SwitchItem* Find(const std::string& key);
  const SwitchItem* Find(const std::string& key) const;
  void Worker();
  bool SendAndWait(SwitchItem* item, bool on, std::string* error);
  bool WaitPayloadAck(uint8_t cmd, int timeout_ms, bool* nack);

  PayloadSwitchConfig cfg_;
  PayloadSendFn payload_send_;
  ResultFn on_result_;
  UdpEndpoint light_;

  SwitchItem items_[5] = {
      {"light", "条纹灯", 0x02, true},
      {"pump", "气泵", 0x04, false},
      {"uwb", "UWB", 0x06, false},
      {"fan", "风扇", 0x07, false},
      {"camera", "摄像头", 0x09, false},
  };

  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool have_job_ = false;
  std::string job_key_;
  bool job_on_ = false;

  bool have_ack_ = false;
  SwitchAck last_ack_{};

  std::string peer_ip_;
  uint16_t peer_port_ = 0;
};

}  // namespace x30
