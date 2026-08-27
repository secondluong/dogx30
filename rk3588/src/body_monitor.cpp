#include "x30/body_monitor.hpp"

#include "x30/net_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#define close_socket closesocket
using io_size_t = int;
#else
#include <sys/socket.h>
#include <unistd.h>
#define close_socket ::close
using io_size_t = ssize_t;
#endif

namespace x30 {
namespace {

bool SendAll(int fd, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (size != 0) {
    const io_size_t n = ::send(fd, reinterpret_cast<const char*>(p),
                               static_cast<int>(size), 0);
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

bool RecvAll(int fd, void* data, size_t size) {
  auto* p = static_cast<uint8_t*>(data);
  while (size != 0) {
    const io_size_t n = ::recv(fd, reinterpret_cast<char*>(p),
                               static_cast<int>(size), 0);
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

bool TagInt(const std::string& xml, const char* tag, int* out) {
  const std::string open = std::string("<") + tag + ">";
  const std::string close = std::string("</") + tag + ">";
  const size_t begin = xml.find(open);
  if (begin == std::string::npos) return false;
  const size_t value_begin = begin + open.size();
  const size_t end = xml.find(close, value_begin);
  if (end == std::string::npos) return false;
  try {
    *out = std::stoi(xml.substr(value_begin, end - value_begin));
    return true;
  } catch (...) {
    return false;
  }
}

bool TagFloat(const std::string& xml, const char* tag, float* out) {
  const std::string open = std::string("<") + tag + ">";
  const std::string close = std::string("</") + tag + ">";
  const size_t begin = xml.find(open);
  if (begin == std::string::npos) return false;
  const size_t value_begin = begin + open.size();
  const size_t end = xml.find(close, value_begin);
  if (end == std::string::npos) return false;
  try {
    *out = std::stof(xml.substr(value_begin, end - value_begin));
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

BodyMonitor::BodyMonitor(BodyMonitorConfig config) : cfg_(std::move(config)) {}

BodyMonitor::~BodyMonitor() { Stop(); }

void BodyMonitor::SetHandler(StateHandler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  handler_ = std::move(handler);
}

void BodyMonitor::Start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread(&BodyMonitor::Loop, this);
}

void BodyMonitor::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

BodyMonitorState BodyMonitor::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void BodyMonitor::Loop() {
  uint16_t request_id = 0;
  bool logged_up = false;
  while (running_.load()) {
    BodyMonitorState next;
    const bool ok = Poll(request_id++, &next);
    next.alive = ok;

    StateHandler handler;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_ = next;
      handler = handler_;
    }
    if (handler) handler(next);

    if (ok && !logged_up) {
      std::printf("[本体监控] 已连接 %s:%u，采用官方 Type=1002 状态\n",
                  cfg_.host.c_str(), cfg_.port);
      logged_up = true;
    } else if (!ok) {
      logged_up = false;
    }

    const int sleep_ms = ok ? cfg_.poll_ms : 1000;
    for (int elapsed = 0; running_.load() && elapsed < sleep_ms;
         elapsed += 50) {
      std::this_thread::sleep_for(std::chrono::milliseconds(
          std::min(50, sleep_ms - elapsed)));
    }
  }
}

bool BodyMonitor::Poll(uint16_t request_id, BodyMonitorState* out) {
  const int fd = TcpConnectTimeout(cfg_.host, cfg_.port,
                                   cfg_.connect_timeout_ms,
                                   cfg_.receive_timeout_ms);
  if (fd < 0) return false;

  static constexpr char kRequest[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<PatrolDevice><Type>1002</Type><Command>1</Command>"
      "<Time>0000-00-00 00:00:00</Time><Items/></PatrolDevice>";
  const uint16_t xml_size = static_cast<uint16_t>(sizeof(kRequest) - 1);
  std::array<uint8_t, 16> head{};
  head[0] = 0xeb;
  head[1] = 0x91;
  head[2] = 0xeb;
  head[3] = 0x90;
  head[4] = static_cast<uint8_t>(xml_size & 0xff);
  head[5] = static_cast<uint8_t>((xml_size >> 8) & 0xff);
  head[6] = static_cast<uint8_t>(request_id & 0xff);
  head[7] = static_cast<uint8_t>((request_id >> 8) & 0xff);

  bool ok = SendAll(fd, head.data(), head.size()) &&
            SendAll(fd, kRequest, xml_size);
  std::array<uint8_t, 16> reply{};
  if (ok) ok = RecvAll(fd, reply.data(), reply.size());
  if (ok) {
    ok = reply[0] == 0xeb && reply[1] == 0x91 &&
         reply[2] == 0xeb && reply[3] == 0x90;
  }
  const uint16_t size =
      static_cast<uint16_t>(reply[4] | (static_cast<uint16_t>(reply[5]) << 8));
  std::string xml(size, '\0');
  if (ok && size != 0) ok = RecvAll(fd, xml.data(), xml.size());
  close_socket(fd);
  if (!ok || size == 0 || xml.find("<Type>1002</Type>") == std::string::npos) {
    return false;
  }

  BodyMonitorState parsed;
  if (!TagInt(xml, "MotionState", &parsed.motion_state)) return false;
  TagInt(xml, "GaitState", &parsed.gait_state);
  TagInt(xml, "MotorState", &parsed.motor_state);
  TagInt(xml, "ChargeState", &parsed.charge_state);
  TagInt(xml, "ControlMode", &parsed.control_mode);
  TagInt(xml, "OnDockState", &parsed.on_dock_state);
  TagInt(xml, "Location", &parsed.location_state);
  TagInt(xml, "Electricity", &parsed.electricity);
  TagFloat(xml, "Speed", &parsed.speed);
  // 官方 V1.0.15 明确规定：Items 中所有参数均为 0，表示查询失败或
  // 定位导航程序未启动，不能把它解释成“机器人确实趴下”。
  if (parsed.motion_state == 0 && parsed.gait_state == 0 &&
      parsed.motor_state == 0 && parsed.charge_state == 0 &&
      parsed.control_mode == 0 && parsed.on_dock_state == 0 &&
      parsed.location_state == 0 && parsed.electricity == 0 &&
      parsed.speed == 0.0f) {
    return false;
  }
  parsed.alive = true;
  *out = parsed;
  return true;
}

}  // namespace x30
