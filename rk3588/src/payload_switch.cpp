#include "x30/payload_switch.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "x30/json.hpp"

namespace x30 {
namespace {

struct Frame {
  const uint8_t* bytes;
  size_t len;
};

const uint8_t kLightOn[] = {0xAA, 0x55, 0x00, 0x0A, 0x02, 0x01, 0xF4, 0xBF,
                            0x0D, 0x0A};
const uint8_t kLightOff[] = {0xAA, 0x55, 0x00, 0x0A, 0x02, 0x00, 0x35, 0x7F,
                             0x0D, 0x0A};
const uint8_t kPumpOn[] = {0xAA, 0x55, 0x00, 0x0A, 0x04, 0x01, 0xF7, 0x1F,
                           0x0D, 0x0A};
const uint8_t kPumpOff[] = {0xAA, 0x55, 0x00, 0x0A, 0x04, 0x00, 0x36, 0xDF,
                            0x0D, 0x0A};
const uint8_t kUwbOn[] = {0xAA, 0x55, 0x00, 0x0A, 0x06, 0x01, 0xF6, 0x7F,
                          0x0D, 0x0A};
const uint8_t kUwbOff[] = {0xAA, 0x55, 0x00, 0x0A, 0x06, 0x00, 0x37, 0xBF,
                           0x0D, 0x0A};
const uint8_t kFanOn[] = {0xAA, 0x55, 0x00, 0x0A, 0x07, 0x01, 0xF7, 0xEF,
                          0x0D, 0x0A};
const uint8_t kFanOff[] = {0xAA, 0x55, 0x00, 0x0A, 0x07, 0x00, 0x36, 0x2F,
                           0x0D, 0x0A};
const uint8_t kCamOn[] = {0xAA, 0x55, 0x00, 0x0A, 0x09, 0x01, 0xF3, 0x8F,
                          0x0D, 0x0A};
const uint8_t kCamOff[] = {0xAA, 0x55, 0x00, 0x0A, 0x09, 0x00, 0x32, 0x4F,
                           0x0D, 0x0A};

Frame CommandFrame(uint8_t cmd, bool on) {
  switch (cmd) {
    case 0x02:
      return on ? Frame{kLightOn, sizeof(kLightOn)}
                : Frame{kLightOff, sizeof(kLightOff)};
    case 0x04:
      return on ? Frame{kPumpOn, sizeof(kPumpOn)}
                : Frame{kPumpOff, sizeof(kPumpOff)};
    case 0x06:
      return on ? Frame{kUwbOn, sizeof(kUwbOn)}
                : Frame{kUwbOff, sizeof(kUwbOff)};
    case 0x07:
      return on ? Frame{kFanOn, sizeof(kFanOn)}
                : Frame{kFanOff, sizeof(kFanOff)};
    case 0x09:
      return on ? Frame{kCamOn, sizeof(kCamOn)}
                : Frame{kCamOff, sizeof(kCamOff)};
    default:
      return Frame{nullptr, 0};
  }
}

}  // namespace

const char* SwitchStatusText(SwitchStatus s) {
  switch (s) {
    case SwitchStatus::kUnknown:
      return "unknown";
    case SwitchStatus::kOn:
      return "on";
    case SwitchStatus::kOff:
      return "off";
    case SwitchStatus::kBusy:
      return "busy";
    case SwitchStatus::kError:
      return "error";
  }
  return "unknown";
}

bool ParseSwitchAck(const uint8_t* data, size_t len, SwitchAck* out) {
  if (data == nullptr || out == nullptr || len < 11) return false;
  for (size_t i = 0; i + 11 <= len; ++i) {
    if (data[i] != 0xAA || data[i + 1] != 0x55) continue;
    if (data[i + 2] != 0x00 || data[i + 3] != 0x0B) continue;
    if (data[i + 9] != 0x0D || data[i + 10] != 0x0A) continue;
    out->cmd = data[i + 4];
    out->ok = data[i + 5] == 0x00;
    out->on = data[i + 6] == 0x01;
    return true;
  }
  return false;
}

PayloadSwitch::PayloadSwitch(PayloadSwitchConfig config)
    : cfg_(std::move(config)) {}

PayloadSwitch::~PayloadSwitch() { Stop(); }

bool PayloadSwitch::Start(std::string* error) {
  if (running_.load()) return true;
  // 灯板应答回源端口，必须有自己的套接字。端口 0 = 内核分配。
  if (!light_.Open(0, error)) return false;
  running_.store(true);
  thread_ = std::thread(&PayloadSwitch::Worker, this);
  return true;
}

void PayloadSwitch::Stop() {
  if (!running_.exchange(false)) {
    light_.Close();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
  }
  light_.Close();
  if (thread_.joinable()) thread_.join();
}

void PayloadSwitch::NotePayloadPeer(const std::string& ip, uint16_t port) {
  if (ip.empty() || port == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  peer_ip_ = ip;
  peer_port_ = port;
}

void PayloadSwitch::OnPayloadAck(const SwitchAck& ack) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_ack_ = ack;
  have_ack_ = true;
  cv_.notify_all();
}

bool PayloadSwitch::Request(const std::string& key, bool on,
                            std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  SwitchItem* item = Find(key);
  if (item == nullptr) {
    if (error) *error = "不认识的开关：" + key;
    return false;
  }
  if (have_job_ || item->status == SwitchStatus::kBusy) {
    if (error) *error = "上一条开关指令还在等应答";
    return false;
  }
  item->status = SwitchStatus::kBusy;
  job_key_ = key;
  job_on_ = on;
  have_job_ = true;
  cv_.notify_all();
  return true;
}

std::string PayloadSwitch::Json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  JsonWriter w;
  w.BeginObject().BeginArray("items");
  for (const auto& it : items_) {
    w.BeginObject()
        .Key("key", it.key)
        .Key("name", it.name)
        .Key("status", SwitchStatusText(it.status))
        .Key("on", it.status == SwitchStatus::kOn)
        .EndObject();
  }
  w.EndArray();
  w.EndObject();
  return w.Take();
}

SwitchItem* PayloadSwitch::Find(const std::string& key) {
  for (auto& it : items_) {
    if (key == it.key) return &it;
  }
  return nullptr;
}

const SwitchItem* PayloadSwitch::Find(const std::string& key) const {
  for (const auto& it : items_) {
    if (key == it.key) return &it;
  }
  return nullptr;
}

void PayloadSwitch::Worker() {
  while (running_.load()) {
    std::string key;
    bool on = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return !running_.load() || have_job_; });
      if (!running_.load()) return;
      key = job_key_;
      on = job_on_;
      have_ack_ = false;
    }

    SwitchItem* item = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      item = Find(key);
    }
    if (item == nullptr) {
      std::lock_guard<std::mutex> lock(mutex_);
      have_job_ = false;
      continue;
    }

    std::string err;
    const bool ok = SendAndWait(item, on, &err);
    const char* status = "error";
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (ok) {
        item->status = on ? SwitchStatus::kOn : SwitchStatus::kOff;
        status = SwitchStatusText(item->status);
      } else {
        item->status = SwitchStatus::kError;
      }
      have_job_ = false;
    }
    if (on_result_) {
      on_result_(key, ok, on, status, ok ? "" : err.c_str());
    }
  }
}

bool PayloadSwitch::WaitPayloadAck(uint8_t cmd, int timeout_ms, bool* nack) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (running_.load() && std::chrono::steady_clock::now() < deadline) {
    cv_.wait_until(lock, deadline, [&] {
      return !running_.load() || (have_ack_ && last_ack_.cmd == cmd);
    });
    if (!running_.load()) return false;
    if (!have_ack_ || last_ack_.cmd != cmd) continue;
    if (last_ack_.ok) return true;
    if (nack) *nack = true;
    have_ack_ = false;
  }
  return false;
}

bool PayloadSwitch::SendAndWait(SwitchItem* item, bool on, std::string* error) {
  const Frame fr = CommandFrame(item->cmd, on);
  if (fr.bytes == nullptr) {
    if (error) *error = "没有这条指令";
    return false;
  }

  // 点一次连发 3 帧、间隔 100ms。灯板要靠前几包叫醒射频；载荷口偶发拒一次
  // 也别把整次操作判死，后两拍成功就算开/关成功。
  constexpr int kBurst = 3;
  constexpr int kGapMs = 100;

  if (item->via_light) {
    const std::string dest =
        cfg_.light_ip.empty() ? std::string("192.168.1.200") : cfg_.light_ip;
    bool sent_ok = false;
    bool nack = false;
    SwitchAck ack;
    for (int i = 0; i < kBurst; ++i) {
      sent_ok = light_.SendTo(dest, cfg_.light_port, fr.bytes, fr.len);
      if (!sent_ok) {
        if (error) *error = "灯板发送失败";
        return false;
      }
      const int wait_ms = (i + 1 < kBurst) ? kGapMs : 2000;
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(wait_ms);
      while (std::chrono::steady_clock::now() < until) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              until - std::chrono::steady_clock::now())
                              .count();
        uint8_t buf[64];
        const int n = light_.Recv(buf, sizeof(buf),
                                  static_cast<int>(left < 1 ? 1 : left));
        if (n > 0 && ParseSwitchAck(buf, static_cast<size_t>(n), &ack) &&
            ack.cmd == item->cmd) {
          if (ack.ok) return true;
          nack = true;
        }
      }
    }
    if (error) *error = nack ? "灯板拒绝" : "灯板无应答";
    return false;
  }

  std::string ip;
  uint16_t port = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ip = !peer_ip_.empty() ? peer_ip_ : cfg_.payload_ip;
    port = peer_port_ != 0 ? peer_port_ : cfg_.payload_port;
    have_ack_ = false;
  }
  if (ip.empty() || port == 0 || !payload_send_) {
    if (error) *error = "载荷主板地址未知";
    return false;
  }

  bool nack = false;
  for (int i = 0; i < kBurst; ++i) {
    if (!payload_send_(ip, port, fr.bytes, fr.len)) {
      if (error) *error = "载荷主板发送失败";
      return false;
    }
    const int wait_ms = (i + 1 < kBurst) ? kGapMs : cfg_.ack_timeout_ms;
    if (WaitPayloadAck(item->cmd, wait_ms, &nack)) return true;
  }
  if (error) *error = nack ? "载荷主板拒绝" : "载荷主板无应答";
  return false;
}

}  // namespace x30
