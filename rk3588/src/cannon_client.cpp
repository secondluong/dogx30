#include "x30/cannon_client.hpp"

#include "x30/json.hpp"
#include "x30/net_util.hpp"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
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

std::atomic<int> g_cannon_held{0};

bool CannonHeld() { return g_cannon_held.load() > 0; }

struct CannonHold {
  CannonHold() { g_cannon_held.fetch_add(1); }
  ~CannonHold() { g_cannon_held.fetch_sub(1); }
};

bool WriteAll(int fd, const uint8_t* data, size_t n) {
  size_t off = 0;
  while (off < n) {
#if defined(_WIN32)
    const int k = ::send(fd, reinterpret_cast<const char*>(data + off),
                         static_cast<int>(n - off), 0);
#else
    const ssize_t k = ::send(fd, data + off, n - off, MSG_NOSIGNAL);
#endif
    if (k <= 0) return false;
    off += static_cast<size_t>(k);
  }
  return true;
}

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
  // 平板正在经 4001 占用炮台的唯一一条 TCP，这里再连会被拒绝。
  if (CannonHeld()) return false;
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

CannonRelay::CannonRelay(std::string host, uint16_t cannon_port,
                         uint16_t listen_port)
    : host_(std::move(host)),
      cannon_port_(cannon_port),
      listen_port_(listen_port) {}

CannonRelay::~CannonRelay() { Stop(); }

void CannonRelay::Start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread(&CannonRelay::Loop, this);
}

void CannonRelay::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void CannonRelay::Serve(int client) {
  CannonHold hold;
  const int cannon = TcpConnectTimeout(host_, cannon_port_, 800, 200);
  if (cannon < 0) {
    std::fprintf(stderr, "水炮转发连不上 %s:%u\n", host_.c_str(),
                 static_cast<unsigned>(cannon_port_));
    closesocket(client);
    return;
  }
  std::printf("水炮转发 %s:%u\n", host_.c_str(),
              static_cast<unsigned>(cannon_port_));
  uint8_t buf[256];
  while (running_.load()) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(client, &read_set);
    FD_SET(cannon, &read_set);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    const int nfds = (client > cannon ? client : cannon) + 1;
    const int ready = ::select(nfds, &read_set, nullptr, nullptr, &tv);
    if (ready < 0) {
#if !defined(_WIN32)
      if (errno == EINTR) continue;
#endif
      break;
    }
    if (ready == 0) continue;
    if (FD_ISSET(client, &read_set)) {
#if defined(_WIN32)
      const int n = ::recv(client, reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
      const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
#endif
      if (n <= 0) break;
      if (!WriteAll(cannon, buf, static_cast<size_t>(n))) break;
    }
    if (FD_ISSET(cannon, &read_set)) {
#if defined(_WIN32)
      const int n = ::recv(cannon, reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
      const ssize_t n = ::recv(cannon, buf, sizeof(buf), 0);
#endif
      if (n <= 0) break;
      if (!WriteAll(client, buf, static_cast<size_t>(n))) break;
    }
  }
  uint8_t stop[kCannonFrameSize];
  CannonCmd idle;
  EncodeCannonFrame(idle, stop);
  WriteAll(cannon, stop, kCannonFrameSize);
  closesocket(cannon);
  closesocket(client);
}

void CannonRelay::Loop() {
  while (running_.load()) {
    const int listen_fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd < 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    const int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(listen_port_);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
            0 ||
        ::listen(listen_fd, 1) != 0) {
      std::fprintf(stderr, "水炮转发听不上 %u\n",
                   static_cast<unsigned>(listen_port_));
      closesocket(listen_fd);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    std::printf("水炮转发监听 0.0.0.0:%u → %s:%u\n",
                static_cast<unsigned>(listen_port_), host_.c_str(),
                static_cast<unsigned>(cannon_port_));
    while (running_.load()) {
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(listen_fd, &read_set);
      timeval tv{};
      tv.tv_sec = 0;
      tv.tv_usec = 200000;
      const int ready = ::select(listen_fd + 1, &read_set, nullptr, nullptr, &tv);
      if (ready < 0) {
#if !defined(_WIN32)
        if (errno == EINTR) continue;
#endif
        break;
      }
      if (ready == 0 || !FD_ISSET(listen_fd, &read_set)) continue;
      const int client = static_cast<int>(::accept(listen_fd, nullptr, nullptr));
      if (client < 0) continue;
      const int nodelay = 1;
      ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
      Serve(client);
    }
    closesocket(listen_fd);
  }
}

}  // namespace x30
