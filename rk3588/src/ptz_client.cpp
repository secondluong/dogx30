#include "x30/ptz_client.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace x30 {
namespace {

// RFC 1321 MD5。球机登录和每条 CGI 都要 user/pwd 的 hex。
std::string Md5Hex(const std::string& input) {
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
      0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
      0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
      0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
      0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
      0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
      0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
      0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
      0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const uint32_t S[64] = {
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
      5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

  const uint64_t bitlen = static_cast<uint64_t>(input.size()) * 8;
  std::string msg = input;
  msg.push_back(static_cast<char>(0x80));
  while ((msg.size() % 64) != 56) msg.push_back(0);
  for (int i = 0; i < 8; ++i) {
    msg.push_back(static_cast<char>((bitlen >> (8 * i)) & 0xff));
  }

  uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  for (size_t off = 0; off < msg.size(); off += 64) {
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
      const unsigned char* p =
          reinterpret_cast<const unsigned char*>(msg.data() + off + i * 4);
      M[i] = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
             (static_cast<uint32_t>(p[2]) << 16) |
             (static_cast<uint32_t>(p[3]) << 24);
    }
    uint32_t A = a0, B = b0, C = c0, D = d0;
    for (int i = 0; i < 64; ++i) {
      uint32_t F, g;
      if (i < 16) {
        F = (B & C) | ((~B) & D);
        g = i;
      } else if (i < 32) {
        F = (D & B) | ((~D) & C);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        F = B ^ C ^ D;
        g = (3 * i + 5) % 16;
      } else {
        F = C ^ (B | (~D));
        g = (7 * i) % 16;
      }
      const uint32_t tmp = D;
      D = C;
      C = B;
      const uint32_t sum = A + F + K[i] + M[g];
      const uint32_t rot = (sum << S[i]) | (sum >> (32 - S[i]));
      B = B + rot;
      A = tmp;
    }
    a0 += A;
    b0 += B;
    c0 += C;
    d0 += D;
  }

  char hex[33];
  const uint32_t words[4] = {a0, b0, c0, d0};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      std::snprintf(hex + i * 8 + j * 2, 3, "%02x",
                    (words[i] >> (8 * j)) & 0xff);
    }
  }
  hex[32] = 0;
  return hex;
}

int ToPct(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  if (std::fabs(v) < 0.08f) return 0;
  return static_cast<int>(std::lround(v * 80.0f));
}

int SpeedFromPct(int pan, int tilt, int zoom) {
  int mag = std::abs(pan);
  if (std::abs(tilt) > mag) mag = std::abs(tilt);
  if (std::abs(zoom) > mag) mag = std::abs(zoom);
  int speed = (mag * 8 + 79) / 80;
  if (speed < 1) speed = 1;
  if (speed > 8) speed = 8;
  return speed;
}

const char* PanTiltAction(int pan, int tilt) {
  const int r = pan > 0 ? 1 : (pan < 0 ? -1 : 0);
  const int u = tilt > 0 ? 1 : (tilt < 0 ? -1 : 0);
  if (u > 0 && r < 0) return "UpLeft";
  if (u > 0 && r > 0) return "UpRight";
  if (u < 0 && r < 0) return "DownLeft";
  if (u < 0 && r > 0) return "DownRight";
  if (u > 0) return "Up";
  if (u < 0) return "Down";
  if (r < 0) return "Left";
  if (r > 0) return "Right";
  return nullptr;
}

std::string ShellSingle(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

int ParseKvInt(const std::string& body, const char* key, int fallback) {
  const std::string needle = std::string(key) + "=";
  size_t pos = 0;
  while ((pos = body.find(needle, pos)) != std::string::npos) {
    if (pos > 0) {
      const char prev = body[pos - 1];
      if (prev != '\n' && prev != '\r') {
        pos += needle.size();
        continue;
      }
    }
    pos += needle.size();
    int sign = 1;
    if (pos < body.size() && body[pos] == '-') {
      sign = -1;
      ++pos;
    }
    int v = 0;
    bool any = false;
    while (pos < body.size() && body[pos] >= '0' && body[pos] <= '9') {
      v = v * 10 + (body[pos] - '0');
      any = true;
      ++pos;
    }
    return any ? sign * v : fallback;
  }
  return fallback;
}

}  // namespace

PtzClient::PtzClient(PtzConfig config) : cfg_(std::move(config)) {
  if (cfg_.user.empty()) cfg_.user = "admin";
  if (cfg_.password.empty() || cfg_.password == "PASSWORD") {
    cfg_.password = "admin";
  }
  // 现场就是 admin/admin。写死这份 hex，避免自写 MD5 和球机对不上。
  if (cfg_.user == "admin" && cfg_.password == "admin") {
    user_md5_ = "21232f297a57a5a743894a0e4a801fc3";
    pwd_md5_ = user_md5_;
  } else {
    user_md5_ = Md5Hex(cfg_.user);
    pwd_md5_ = Md5Hex(cfg_.password);
  }
}

PtzClient::~PtzClient() { Stop(); }

void PtzClient::Start() {
  if (cfg_.host.empty() || running_.exchange(true)) return;
  Login();
  thread_ = std::thread(&PtzClient::Loop, this);
  std::printf("[ptz] 布控球 anv %s:%u 账号 %s\n", cfg_.host.c_str(),
              static_cast<unsigned>(cfg_.port), cfg_.user.c_str());
}

void PtzClient::Stop() {
  if (!running_.exchange(false)) return;
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  SendMove(0, 0, 0);
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
        SendMove(0, 0, 0);
        sent_stop = true;
        last_p = last_t = last_z = 0;
      }
      continue;
    }
    if (p != last_p || t != last_t || z != last_z) {
      SendMove(p, t, z);
      last_p = p;
      last_t = t;
      last_z = z;
      sent_stop = false;
    }
  }
}

std::string PtzClient::CgiGet(const std::string& cgi, const std::string& query) {
  if (cfg_.host.empty()) return "";
  char url[768];
  const int n = std::snprintf(
      url, sizeof(url),
      "http://%s:%u/cgi-bin/anv/%s?user=%s&pwd=%s&%s",
      cfg_.host.c_str(), static_cast<unsigned>(cfg_.port), cgi.c_str(),
      user_md5_.c_str(), pwd_md5_.c_str(), query.c_str());
  if (n < 0 || static_cast<size_t>(n) >= sizeof(url)) return "";

  char cmd[1024];
  const int m = std::snprintf(
      cmd, sizeof(cmd),
      "curl -sS --connect-timeout 0.5 --max-time 1.2 %s",
      ShellSingle(url).c_str());
  if (m < 0 || static_cast<size_t>(m) >= sizeof(cmd)) return "";

  FILE* pipe = popen(cmd, "r");
  if (!pipe) {
    if (!warned_) {
      warned_ = true;
      std::fprintf(stderr, "[ptz] 球机 CGI 拉不起 curl\n");
    }
    return "";
  }
  std::string out;
  char buf[256];
  while (fgets(buf, sizeof(buf), pipe)) out += buf;
  const int st = pclose(pipe);
  if (st != 0 && !warned_) {
    warned_ = true;
    std::fprintf(stderr, "[ptz] 球机未响应（检查双光 RTSP 主机和 admin 口令）\n");
  }
  return out;
}

bool PtzClient::CgiOk(const std::string& cgi, const std::string& query) {
  const std::string body = CgiGet(cgi, query);
  return body.size() >= 2 && body.compare(0, 2, "OK") == 0;
}

void PtzClient::Login() {
  CgiOk("login_cgi", "Cache=1");
}

void PtzClient::SendMove(int pan, int tilt, int zoom) {
  if (cfg_.host.empty()) return;
  const int speed = (pan == 0 && tilt == 0 && zoom == 0)
                        ? 1
                        : SpeedFromPct(pan, tilt, zoom);
  if (pan == 0 && tilt == 0 && zoom == 0) {
    CgiOk("ptz_cgi", "action=Stop&Speed=1");
    return;
  }
  if (const char* dir = PanTiltAction(pan, tilt)) {
    char q[80];
    std::snprintf(q, sizeof(q), "action=%s&Speed=%d", dir, speed);
    CgiOk("ptz_cgi", q);
  }
  if (zoom != 0) {
    char q[80];
    std::snprintf(q, sizeof(q), "action=%s&Speed=%d",
                  zoom > 0 ? "ZoomAdd" : "ZoomSub", speed);
    CgiOk("ptz_cgi", q);
  }
}

PtzPip PtzClient::GetPip() {
  PtzPip out;
  const std::string body = CgiGet("pip_cgi", "action=get");
  if (body.find("mode=") == std::string::npos) return out;
  out.mode = ParseKvInt(body, "mode", 0);
  out.size = ParseKvInt(body, "SmallPicSize", 0);
  out.pos = ParseKvInt(body, "SmallPicPos", 0);
  out.x = ParseKvInt(body, "CustomSmallPicX", 0);
  out.y = ParseKvInt(body, "CustomSmallPicY", 0);
  out.ok = true;
  std::lock_guard<std::mutex> lock(mutex_);
  pip_ = out;
  return out;
}

PtzPip PtzClient::SetPip(int mode, int size, int pos) {
  PtzPip cur = GetPip();
  if (mode < 0) mode = cur.ok ? cur.mode : 0;
  if (mode > 5) mode = 5;
  if (size < 0) size = cur.ok ? cur.size : 0;
  if (pos < 0) pos = cur.ok ? cur.pos : 0;
  const int x = cur.ok ? cur.x : 0;
  const int y = cur.ok ? cur.y : 0;
  char q[160];
  std::snprintf(q, sizeof(q),
                "action=set&mode=%d&SmallPicSize=%d&SmallPicPos=%d"
                "&CustomSmallPicX=%d&CustomSmallPicY=%d",
                mode, size, pos, x, y);
  CgiGet("pip_cgi", q);
  return GetPip();
}

}  // namespace x30
