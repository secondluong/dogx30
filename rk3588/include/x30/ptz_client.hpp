// 布控球云台：EVERET / anv CGI（不是海康 ISAPI）。
// 摇杆量由遥控端按 10 Hz 送来，本类在独立线程里发给球机，
// 避免卡在 WebSocket 回调里拖死急停。
//
// 现场球机 https://192.168.1.168 账号 admin/admin：
//   /cgi-bin/anv/ptz_cgi?action=Left|Right|Up|Down|ZoomAdd|ZoomSub|Stop&user=MD5&pwd=MD5&Speed=1..8
//   /cgi-bin/anv/pip_cgi?action=get|set&mode=&SmallPicSize=&SmallPicPos=

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
  std::string password = "admin";
  int channel = 1;
};

struct PtzPip {
  int mode = 0;
  int size = 0;
  int pos = 0;
  int x = 0;
  int y = 0;
  bool ok = false;
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

  // size/pos < 0 时沿用球机当前值，避免 App 只切模式时把副图尺寸改掉。
  PtzPip SetPip(int mode, int size = -1, int pos = -1);
  PtzPip GetPip();

  bool configured() const { return !cfg_.host.empty(); }

 private:
  void Loop();
  void SendMove(int pan, int tilt, int zoom);
  void Login();
  std::string CgiGet(const std::string& cgi, const std::string& query);
  bool CgiOk(const std::string& cgi, const std::string& query);

  PtzConfig cfg_;
  std::string user_md5_;
  std::string pwd_md5_;
  std::mutex mutex_;
  std::condition_variable wake_;
  float pan_ = 0, tilt_ = 0, zoom_ = 0;
  bool dirty_ = false;
  std::atomic<bool> running_{false};
  std::thread thread_;
  bool warned_ = false;
  PtzPip pip_{};
};

}  // namespace x30
