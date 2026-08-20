// 布控球云台：海康 ISAPI 连续转动。摇杆量由遥控端按 10 Hz 送来，
// 本类在独立线程里发给球机，避免卡在 WebSocket 回调里拖死急停。

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace x30 {

struct PtzConfig {
  std::string host;
  uint16_t port = 80;
  std::string user = "admin";
  std::string password;
  int channel = 1;
};

class PtzClient {
 public:
  explicit PtzClient(PtzConfig config);
  ~PtzClient();

  PtzClient(const PtzClient&) = delete;
  PtzClient& operator=(const PtzClient&) = delete;

  void Start();
  void Stop();

  // 各轴 -1..1。全零会补一帧停止。
  void Set(float pan, float tilt, float zoom);
  void StopMove();

  bool configured() const { return !cfg_.host.empty(); }

 private:
  void Loop();
  void Send(int pan, int tilt, int zoom);

  PtzConfig cfg_;
  std::mutex mutex_;
  std::condition_variable wake_;
  float pan_ = 0, tilt_ = 0, zoom_ = 0;
  bool dirty_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;
  bool warned_ = false;
};

}  // namespace x30
