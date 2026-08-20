#include "x30/ptz_client.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace x30 {
namespace {

int ToPct(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  if (std::fabs(v) < 0.08f) return 0;
  return static_cast<int>(std::lround(v * 80.0f));
}

}  // namespace

PtzClient::PtzClient(PtzConfig config) : cfg_(std::move(config)) {}

PtzClient::~PtzClient() { Stop(); }

void PtzClient::Start() {
  if (cfg_.host.empty() || running_.exchange(true)) return;
  thread_ = std::thread(&PtzClient::Loop, this);
  std::printf("[ptz] 布控球 %s:%u 通道 %d\n", cfg_.host.c_str(),
              static_cast<unsigned>(cfg_.port), cfg_.channel);
}

void PtzClient::Stop() {
  if (!running_.exchange(false)) return;
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  Send(0, 0, 0);
}

void PtzClient::Set(float pan, float tilt, float zoom) {
  std::lock_guard<std::mutex> lock(mutex_);
  pan_ = pan;
  tilt_ = tilt;
  zoom_ = zoom;
  dirty_ = true;
  wake_.notify_all();
}

void PtzClient::StopMove() { Set(0, 0, 0); }

void PtzClient::Loop() {
  int last_p = 0, last_t = 0, last_z = 0;
  bool sent_stop = true;
  while (running_.load()) {
    float pan = 0, tilt = 0, zoom = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(120),
                     [this] { return !running_.load() || dirty_; });
      if (!running_.load()) break;
      pan = pan_;
      tilt = tilt_;
      zoom = zoom_;
      dirty_ = false;
    }
    const int p = ToPct(pan), t = ToPct(tilt), z = ToPct(zoom);
    if (p == 0 && t == 0 && z == 0) {
      if (!sent_stop) {
        Send(0, 0, 0);
        sent_stop = true;
        last_p = last_t = last_z = 0;
      }
      continue;
    }
    if (p != last_p || t != last_t || z != last_z) {
      Send(p, t, z);
      last_p = p;
      last_t = t;
      last_z = z;
      sent_stop = false;
    }
  }
}

void PtzClient::Send(int pan, int tilt, int zoom) {
  if (cfg_.host.empty()) return;
  char cmd[768];
  const int n = std::snprintf(
      cmd, sizeof(cmd),
      "curl -sS --connect-timeout 0.4 --max-time 0.7 --anyauth "
      "-u '%s:%s' -X PUT -H 'Content-Type: application/xml' "
      "--data-binary "
      "'<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<PTZData><pan>%d</pan><tilt>%d</tilt><zoom>%d</zoom></PTZData>' "
      "'http://%s:%u/ISAPI/PTZCtrl/channels/%d/continuous' "
      ">/dev/null 2>&1",
      cfg_.user.c_str(), cfg_.password.c_str(), pan, tilt, zoom,
      cfg_.host.c_str(), static_cast<unsigned>(cfg_.port), cfg_.channel);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(cmd)) return;
  const int st = std::system(cmd);
  if (st != 0 && !warned_) {
    warned_ = true;
    std::fprintf(stderr, "[ptz] 球机未响应（检查设置里的白光/热成像 RTSP 口令）\n");
  }
}

}  // namespace x30
