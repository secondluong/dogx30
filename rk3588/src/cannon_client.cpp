#include "x30/cannon_client.hpp"

#include "x30/json.hpp"
#include "x30/net_util.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket ::close
#endif

namespace x30 {
namespace {

constexpr float kDead = 0.08f;
constexpr int kPeriodMs = 100;
constexpr int kStaleMs = 400;
constexpr int kConnectMs = 800;
constexpr int kRecvMs = 40;

bool AxisOn(float v) { return std::fabs(v) > kDead; }

void CopySpray(char* dst, const std::string& value) {
  const char* src = "";
  if (value == "fog") src = "fog";
  else if (value == "jet") src = "jet";
  std::memset(dst, 0, 8);
  std::strncpy(dst, src, 7);
}

}  // namespace

void EncodeCannonFrame(const CannonCmd& cmd, uint8_t out[kCannonFrameSize]) {
  std::memset(out, 0, kCannonFrameSize);
  std::memcpy(out, kCannonHdr, 5);
  uint8_t d0 = 0;
  uint8_t d2 = 0;
  if (AxisOn(cmd.tilt)) {
    if (cmd.tilt > 0) d0 |= 0x02;  // 上
    else d0 |= 0x04;               // 下
  }
  if (AxisOn(cmd.pan)) {
    if (cmd.pan < 0) d0 |= 0x08;  // 左
    else d0 |= 0x10;              // 右
  }
  if (std::strcmp(cmd.spray, "fog") == 0) d0 |= 0x20;
  else if (std::strcmp(cmd.spray, "jet") == 0) d0 |= 0x40;
  if (cmd.fire) d2 |= 0x02;  // 开阀
  out[5] = d0;
  out[7] = d2;
}

bool CannonFrameIdle(const uint8_t frame[kCannonFrameSize]) {
  if (!frame) return true;
  for (size_t i = 5; i < kCannonFrameSize; ++i) {
    if (frame[i] != 0) return false;
  }
  return true;
}

bool ParseCannonReply(const uint8_t* data, size_t len, CannonTelem* out) {
  if (!data || !out || len < kCannonFrameSize) return false;
  if (data[0] != 0x08 || data[1] != 0x00 || data[2] != 0x00) return false;
  const uint8_t kind = data[3];
  const uint8_t* d = data + 5;
  if (kind == 0x01) {
    out->have_pose = true;
    out->pan_deg = d[0];
    out->pan_dir = d[1];
    out->tilt_deg = d[2];
    out->tilt_dir = d[3];
    out->limit = d[4];
    return true;
  }
  if (kind == 0x03) {
    const uint16_t raw = static_cast<uint16_t>(d[0]) |
                         (static_cast<uint16_t>(d[1]) << 8);
    out->have_pressure = true;
    out->pressure_mpa = static_cast<float>(raw) / 2500.0f;
    return true;
  }
  return false;
}

CannonClient::CannonClient(CannonConfig config) : cfg_(std::move(config)) {}

CannonClient::~CannonClient() { Stop(); }

void CannonClient::Start() {
  if (running_.exchange(true)) return;
  last_cmd_ = std::chrono::steady_clock::now();
  thread_ = std::thread(&CannonClient::Loop, this);
}

void CannonClient::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) {
    uint8_t stop[kCannonFrameSize];
    CannonCmd idle;
    EncodeCannonFrame(idle, stop);
    SendFrame(stop);
  }
  CloseFd();
}

void CannonClient::SetAim(float pan, float tilt) {
  std::lock_guard<std::mutex> lock(mutex_);
  cmd_.pan = pan;
  cmd_.tilt = tilt;
  last_cmd_ = std::chrono::steady_clock::now();
}

void CannonClient::SetSpray(const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  CopySpray(cmd_.spray, value);
  last_cmd_ = std::chrono::steady_clock::now();
}

void CannonClient::SetFire(bool on) {
  std::lock_guard<std::mutex> lock(mutex_);
  cmd_.fire = on;
  last_cmd_ = std::chrono::steady_clock::now();
}

void CannonClient::Idle() {
  std::lock_guard<std::mutex> lock(mutex_);
  cmd_ = CannonCmd{};
  last_cmd_ = std::chrono::steady_clock::now();
}

CannonCmd CannonClient::Snapshot(bool* stale) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto age = std::chrono::steady_clock::now() - last_cmd_;
  *stale = age > std::chrono::milliseconds(kStaleMs);
  return cmd_;
}

bool CannonClient::EnsureConnected() {
  if (fd_ >= 0) return true;
  if (cfg_.host.empty() || cfg_.port == 0) return false;
  fd_ = TcpConnectTimeout(cfg_.host, cfg_.port, kConnectMs, kRecvMs);
  if (fd_ < 0) {
    if (!warned_) {
      warned_ = true;
      std::fprintf(stderr, "水炮未连上 %s:%u（炮台 TCP，不影响网关其它功能）\n",
                    cfg_.host.c_str(), static_cast<unsigned>(cfg_.port));
    }
    online_.store(false);
    return false;
  }
  const int one = 1;
  ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));
  online_.store(true);
  warned_ = false;
  rx_len_ = 0;
  std::printf("水炮已连 %s:%u\n", cfg_.host.c_str(),
              static_cast<unsigned>(cfg_.port));
  return true;
}

void CannonClient::CloseFd() {
  if (fd_ >= 0) {
    closesocket(fd_);
    fd_ = -1;
  }
  online_.store(false);
}

bool CannonClient::SendFrame(const uint8_t frame[kCannonFrameSize]) {
  if (fd_ < 0) return false;
#if defined(_WIN32)
  const int n = ::send(fd_, reinterpret_cast<const char*>(frame),
                       static_cast<int>(kCannonFrameSize), 0);
#else
  const ssize_t n = ::send(fd_, frame, kCannonFrameSize, 0);
#endif
  if (n != static_cast<int>(kCannonFrameSize)) {
    CloseFd();
    return false;
  }
  return true;
}

void CannonClient::DrainReplies() {
  if (fd_ < 0) return;
  uint8_t buf[32];
#if defined(_WIN32)
  const int n = ::recv(fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
  const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
#endif
  if (n <= 0) return;
  if (rx_len_ + static_cast<size_t>(n) > sizeof(rx_)) rx_len_ = 0;
  std::memcpy(rx_ + rx_len_, buf, static_cast<size_t>(n));
  rx_len_ += static_cast<size_t>(n);
  size_t i = 0;
  while (i + kCannonFrameSize <= rx_len_) {
    if (rx_[i] != 0x08) {
      ++i;
      continue;
    }
    CannonTelem t;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      t = telem_;
    }
    if (ParseCannonReply(rx_ + i, kCannonFrameSize, &t)) {
      std::lock_guard<std::mutex> lock(mutex_);
      telem_ = t;
    }
    i += kCannonFrameSize;
  }
  if (i > 0 && i < rx_len_) {
    std::memmove(rx_, rx_ + i, rx_len_ - i);
    rx_len_ -= i;
  } else if (i >= rx_len_) {
    rx_len_ = 0;
  }
}

void CannonClient::Loop() {
  while (running_.load()) {
    bool stale = false;
    CannonCmd cmd = Snapshot(&stale);
    if (stale) cmd = CannonCmd{};

    uint8_t frame[kCannonFrameSize];
    EncodeCannonFrame(cmd, frame);
    const bool idle = CannonFrameIdle(frame);

    if (!idle || !sent_idle_) {
      if (EnsureConnected()) {
        if (SendFrame(frame)) sent_idle_ = idle;
      }
    }

    DrainReplies();
    std::this_thread::sleep_for(std::chrono::milliseconds(kPeriodMs));
  }
}

std::string CannonClient::Json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const char* spray = cmd_.spray[0] ? cmd_.spray : "off";
  JsonWriter w;
  w.BeginObject()
      .Key("online", online_.load())
      .Key("fire", cmd_.fire)
      .Key("spray", spray)
      .Key("pan", cmd_.pan)
      .Key("tilt", cmd_.tilt)
      .Key("have_pose", telem_.have_pose)
      .Key("pan_deg", static_cast<int>(telem_.pan_deg))
      .Key("tilt_deg", static_cast<int>(telem_.tilt_deg))
      .Key("have_pressure", telem_.have_pressure)
      .Key("pressure_mpa", telem_.pressure_mpa, 3)
      .EndObject();
  return w.Take();
}

}  // namespace x30
