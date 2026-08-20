#include "x30/gateway_config.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "x30/json.hpp"

namespace x30 {
namespace {

std::string Trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

bool IsIpv4(const std::string& ip) {
  in_addr addr{};
  return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

bool ParseLong(const std::string& text, long* out) {
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0') return false;
  *out = v;
  return true;
}

std::string NormalizeVideoCodec(const std::string& raw) {
  std::string s = raw;
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s == "h.264" || s == "avc") return "h264";
  if (s == "h.265" || s == "hevc") return "h265";
  return s;
}

bool ParseBool(const std::string& text, bool* out) {
  if (text == "yes" || text == "true" || text == "1" || text == "on") {
    *out = true;
    return true;
  }
  if (text == "no" || text == "false" || text == "0" || text == "off") {
    *out = false;
    return true;
  }
  return false;
}

// 端口 0 有效但没意义（内核随机分配），对这套系统而言一定是填错了。
bool ValidPort(long v) { return v >= 1 && v <= 65535; }

// 试探能否绑上。换监听地址或端口时先验一遍，否则重启后 bind 失败，
// systemd 会 1 秒一次地反复重启，而控制台已经没了，只能上命令行救。
bool CanBind(const std::string& ip, uint16_t port, std::string* why) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    *why = "创建套接字失败";
    return false;
  }
  // 不设 SO_REUSEADDR：这里要问的正是"有没有别的进程正占着"，
  // 设了反而会把冲突掩盖过去。
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    *why = "地址格式不对";
    return false;
  }
  const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr),
                         sizeof(addr)) == 0;
  if (!ok) *why = std::strerror(errno);
  ::close(fd);
  return ok;
}

// ROS master 形如 http://<主机>:<端口>。这里只做够用的检查：能取出主机和
// 端口，端口在范围内。写成 https 或漏掉端口都是真实会犯的错。
bool ValidRosMaster(const std::string& uri, std::string* error) {
  const std::string prefix = "http://";
  if (uri.compare(0, prefix.size(), prefix) != 0) {
    *error = "ROS master 必须以 http:// 开头（ROS1 的 XML-RPC 没有 TLS）";
    return false;
  }
  const std::string rest = uri.substr(prefix.size());
  const size_t colon = rest.find(':');
  if (colon == std::string::npos || colon == 0) {
    *error = "ROS master 缺少主机或端口，形如 http://192.168.1.105:11311";
    return false;
  }
  std::string port_text = rest.substr(colon + 1);
  // 允许结尾的 "/"，roscore 打印出来的地址就带一个，照抄很常见。
  while (!port_text.empty() && port_text.back() == '/') port_text.pop_back();
  long port = 0;
  if (!ParseLong(port_text, &port) || !ValidPort(port)) {
    *error = "ROS master 的端口不合法，通常是 11311";
    return false;
  }
  return true;
}

// --- JSON 取值：类型不符一律报错，不静默沿用旧值 ---------------------------

bool TakeString(const Json& obj, const char* key, std::string* out,
                std::string* error) {
  if (!obj.Has(key)) return true;
  const Json& v = obj[key];
  if (v.type() != Json::Type::kString) {
    *error = std::string(key) + " 必须是字符串";
    return false;
  }
  const std::string text = Trim(v.AsString());
  if (text.empty()) {
    *error = std::string(key) + " 不能为空";
    return false;
  }
  *out = text;
  return true;
}

bool TakeLong(const Json& obj, const char* key, long lo, long hi, long* out,
              std::string* error) {
  if (!obj.Has(key)) return true;
  const Json& v = obj[key];
  if (v.type() != Json::Type::kNumber) {
    *error = std::string(key) + " 必须是数字";
    return false;
  }
  const double d = v.AsNumber();
  const long l = static_cast<long>(d);
  if (static_cast<double>(l) != d || l < lo || l > hi) {
    *error = std::string(key) + " 超出允许范围（" + std::to_string(lo) + "–" +
             std::to_string(hi) + "）";
    return false;
  }
  *out = l;
  return true;
}

bool TakeOptionalString(const Json& obj, const char* key, std::string* out,
                        std::string* error) {
  if (!obj.Has(key)) return true;
  const Json& v = obj[key];
  if (v.type() != Json::Type::kString) {
    *error = std::string(key) + " 必须是字符串";
    return false;
  }
  *out = Trim(v.AsString());
  return true;
}

bool TakeBool(const Json& obj, const char* key, bool* out,
              std::string* error) {
  if (!obj.Has(key)) return true;
  const Json& v = obj[key];
  if (v.type() != Json::Type::kBool) {
    *error = std::string(key) + " 必须是布尔值";
    return false;
  }
  *out = v.AsBool();
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 读写
// ---------------------------------------------------------------------------

ConfigLoad LoadGatewaySettings(const std::string& path, GatewaySettings* out,
                               std::string* error) {
  std::ifstream in(path);
  if (!in.is_open()) return ConfigLoad::kMissing;

  GatewaySettings s;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;

    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      *error = "第 " + std::to_string(lineno) + " 行不是 key = value：" + trimmed;
      return ConfigLoad::kMalformed;
    }
    const std::string key = Trim(trimmed.substr(0, eq));
    const std::string val = Trim(trimmed.substr(eq + 1));

    auto bad = [&](const char* what) {
      *error = "第 " + std::to_string(lineno) + " 行 " + key + " " + what +
               "：" + val;
      return ConfigLoad::kMalformed;
    };

    long num = 0;
    if (key == "robot_ip") {
      s.robot_ip = val;
    } else if (key == "robot_port") {
      if (!ParseLong(val, &num) || !ValidPort(num)) return bad("不是合法端口");
      s.robot_port = static_cast<uint16_t>(num);
    } else if (key == "local_port") {
      if (!ParseLong(val, &num) || !ValidPort(num)) return bad("不是合法端口");
      s.local_port = static_cast<uint16_t>(num);
    } else if (key == "perception_ip") {
      s.perception_ip = val;
    } else if (key == "perception_port") {
      if (!ParseLong(val, &num) || !ValidPort(num)) return bad("不是合法端口");
      s.perception_port = static_cast<uint16_t>(num);
    } else if (key == "http_port") {
      if (!ParseLong(val, &num) || !ValidPort(num)) return bad("不是合法端口");
      s.http_port = static_cast<uint16_t>(num);
    } else if (key == "bind_address") {
      s.bind_address = val;
    } else if (key == "cloud_enabled") {
      if (!ParseBool(val, &s.cloud_enabled)) return bad("不是 yes/no");
    } else if (key == "ros_master") {
      s.ros_master = val;
    } else if (key == "ros_host") {
      s.ros_host = val;
    } else if (key == "cloud_topic") {
      s.cloud_topic = val;
    } else if (key == "cloud_hz") {
      if (!ParseLong(val, &num) || num < 1 || num > 30) return bad("应在 1–30");
      s.cloud_hz = static_cast<int>(num);
    } else if (key == "cloud_points") {
      if (!ParseLong(val, &num) || num < 100 || num > 200000)
        return bad("应在 100–200000");
      s.cloud_points = static_cast<uint32_t>(num);
    } else if (key == "ptz_vis_rtsp") {
      s.ptz_vis_rtsp = val;
    } else if (key == "ptz_ir_rtsp") {
      s.ptz_ir_rtsp = val;
    } else if (key == "ptz_vis_codec") {
      s.ptz_vis_codec = NormalizeVideoCodec(val);
    } else if (key == "ptz_ir_codec") {
      s.ptz_ir_codec = NormalizeVideoCodec(val);
    } else {
      // 未知键报错而不是忽略。拼错 robot_ip 却被静默忽略的话，网关会拿
      // 默认地址去跑，而文件里明明写着正确的地址 —— 这种现场没人查得出来。
      *error = "第 " + std::to_string(lineno) + " 行是无法识别的配置项：" + key;
      return ConfigLoad::kMalformed;
    }
  }

  *out = s;
  return ConfigLoad::kOk;
}

bool SaveGatewaySettings(const std::string& path, const GatewaySettings& s,
                         std::string* error) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (f == nullptr) {
    *error = "打不开 " + tmp + "：" + std::strerror(errno);
    return false;
  }

  // 每次都重写完整的一份，包括这段说明。控制台保存后注释不会保留，
  // 所以说明必须由程序自己写回去，否则文件用两次就变成一堆裸键值。
  std::fprintf(f,
               "# X30 网关配置。控制台「设置」面板会重写本文件，\n"
               "# 手写的注释不会保留。改完需要重启服务才生效：\n"
               "#   systemctl restart x30-gateway\n"
               "\n");
  std::fprintf(f, "robot_ip = %s\n", s.robot_ip.c_str());
  std::fprintf(f, "robot_port = %u\n", s.robot_port);
  std::fprintf(f, "local_port = %u\n", s.local_port);
  std::fprintf(f, "\n");
  std::fprintf(f, "perception_ip = %s\n", s.perception_ip.c_str());
  std::fprintf(f, "perception_port = %u\n", s.perception_port);
  std::fprintf(f, "\n");
  std::fprintf(f, "http_port = %u\n", s.http_port);
  std::fprintf(f, "bind_address = %s\n", s.bind_address.c_str());
  std::fprintf(f, "\n");
  std::fprintf(f, "cloud_enabled = %s\n", s.cloud_enabled ? "yes" : "no");
  std::fprintf(f, "ros_master = %s\n", s.ros_master.c_str());
  std::fprintf(f, "ros_host = %s\n", s.ros_host.c_str());
  std::fprintf(f, "cloud_topic = %s\n", s.cloud_topic.c_str());
  std::fprintf(f, "cloud_hz = %d\n", s.cloud_hz);
  std::fprintf(f, "cloud_points = %u\n", s.cloud_points);
  std::fprintf(f, "\n");
  std::fprintf(f, "ptz_vis_rtsp = %s\n", s.ptz_vis_rtsp.c_str());
  std::fprintf(f, "ptz_ir_rtsp = %s\n", s.ptz_ir_rtsp.c_str());
  std::fprintf(f, "ptz_vis_codec = %s\n", s.ptz_vis_codec.c_str());
  std::fprintf(f, "ptz_ir_codec = %s\n", s.ptz_ir_codec.c_str());

  if (std::fflush(f) != 0 || ::fsync(::fileno(f)) != 0) {
    *error = std::string("写入 ") + tmp + " 失败：" + std::strerror(errno);
    std::fclose(f);
    ::unlink(tmp.c_str());
    return false;
  }
  std::fclose(f);

  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    *error = "替换 " + path + " 失败：" + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }

  // 目录项也要落盘，否则断电后可能只剩下旧的那份甚至什么都不剩。
  const size_t slash = path.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

// ---------------------------------------------------------------------------
// 协议对接
// ---------------------------------------------------------------------------

bool MergeGatewaySettings(const Json& obj, GatewaySettings* inout,
                          std::string* error) {
  if (obj.type() != Json::Type::kObject) {
    *error = "缺少 settings 对象";
    return false;
  }

  // 只认已知键，其余一律报错。控制台是网关自己托管的，版本必然一致，
  // 所以出现不认识的键只可能是拼错了 —— 静默忽略的表现是
  // 「改了、保存了、没生效、也没报错」，那是最难查的一类。
  static const char* kKnown[] = {
      "robot_ip",      "robot_port",      "local_port",   "perception_ip",
      "perception_port", "http_port",     "bind_address", "cloud_enabled",
      "ros_master",    "ros_host",        "cloud_topic",  "cloud_hz",
      "cloud_points",  "ptz_vis_rtsp",    "ptz_ir_rtsp",
      "ptz_vis_codec", "ptz_ir_codec"};
  for (const auto& key : obj.Keys()) {
    bool known = false;
    for (const char* k : kKnown) {
      if (key == k) {
        known = true;
        break;
      }
    }
    if (!known) {
      *error = "不认识的配置项：" + key;
      return false;
    }
  }

  GatewaySettings s = *inout;
  long num = 0;

  if (!TakeString(obj, "robot_ip", &s.robot_ip, error)) return false;
  num = s.robot_port;
  if (!TakeLong(obj, "robot_port", 1, 65535, &num, error)) return false;
  s.robot_port = static_cast<uint16_t>(num);
  num = s.local_port;
  if (!TakeLong(obj, "local_port", 1, 65535, &num, error)) return false;
  s.local_port = static_cast<uint16_t>(num);

  if (!TakeString(obj, "perception_ip", &s.perception_ip, error)) return false;
  num = s.perception_port;
  if (!TakeLong(obj, "perception_port", 1, 65535, &num, error)) return false;
  s.perception_port = static_cast<uint16_t>(num);

  num = s.http_port;
  if (!TakeLong(obj, "http_port", 1, 65535, &num, error)) return false;
  s.http_port = static_cast<uint16_t>(num);
  if (!TakeString(obj, "bind_address", &s.bind_address, error)) return false;

  if (!TakeBool(obj, "cloud_enabled", &s.cloud_enabled, error)) return false;
  if (!TakeString(obj, "ros_master", &s.ros_master, error)) return false;
  if (!TakeString(obj, "ros_host", &s.ros_host, error)) return false;
  if (!TakeString(obj, "cloud_topic", &s.cloud_topic, error)) return false;
  num = s.cloud_hz;
  if (!TakeLong(obj, "cloud_hz", 1, 30, &num, error)) return false;
  s.cloud_hz = static_cast<int>(num);
  num = s.cloud_points;
  if (!TakeLong(obj, "cloud_points", 100, 200000, &num, error)) return false;
  s.cloud_points = static_cast<uint32_t>(num);

  if (!TakeOptionalString(obj, "ptz_vis_rtsp", &s.ptz_vis_rtsp, error))
    return false;
  if (!TakeOptionalString(obj, "ptz_ir_rtsp", &s.ptz_ir_rtsp, error))
    return false;
  if (!TakeOptionalString(obj, "ptz_vis_codec", &s.ptz_vis_codec, error))
    return false;
  s.ptz_vis_codec = NormalizeVideoCodec(s.ptz_vis_codec);
  if (!TakeOptionalString(obj, "ptz_ir_codec", &s.ptz_ir_codec, error))
    return false;
  s.ptz_ir_codec = NormalizeVideoCodec(s.ptz_ir_codec);

  *inout = s;
  return true;
}

std::string GatewaySettingsJson(const GatewaySettings& s) {
  JsonWriter w;
  w.BeginObject()
      .Key("robot_ip", s.robot_ip)
      .Key("robot_port", static_cast<int>(s.robot_port))
      .Key("local_port", static_cast<int>(s.local_port))
      .Key("perception_ip", s.perception_ip)
      .Key("perception_port", static_cast<int>(s.perception_port))
      .Key("http_port", static_cast<int>(s.http_port))
      .Key("bind_address", s.bind_address)
      .Key("cloud_enabled", s.cloud_enabled)
      .Key("ros_master", s.ros_master)
      .Key("ros_host", s.ros_host)
      .Key("cloud_topic", s.cloud_topic)
      .Key("cloud_hz", s.cloud_hz)
      .Key("cloud_points", static_cast<int>(s.cloud_points))
      .Key("ptz_vis_rtsp", s.ptz_vis_rtsp)
      .Key("ptz_ir_rtsp", s.ptz_ir_rtsp)
      .Key("ptz_vis_codec", s.ptz_vis_codec)
      .Key("ptz_ir_codec", s.ptz_ir_codec)
      .EndObject();
  return w.Take();
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

bool IsLocalIpv4(const std::string& ip) {
  in_addr want{};
  if (inet_pton(AF_INET, ip.c_str(), &want) != 1) return false;

  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) return false;

  bool found = false;
  for (ifaddrs* it = list; it != nullptr && !found; it = it->ifa_next) {
    if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET) continue;
    const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
    if (sin->sin_addr.s_addr == want.s_addr) found = true;
  }
  ::freeifaddrs(list);
  return found;
}

bool ValidateGatewaySettings(const GatewaySettings& s,
                             const GatewaySettings& current,
                             std::string* error) {
  struct Peer {
    const char* label;
    const std::string& ip;
  };
  const Peer peers[] = {{"运动主机地址", s.robot_ip},
                        {"感知主机地址", s.perception_ip}};
  for (const auto& p : peers) {
    if (!IsIpv4(p.ip)) {
      *error = std::string(p.label) + " 不是合法的 IPv4 地址：" + p.ip;
      return false;
    }
    if (p.ip == "0.0.0.0") {
      *error = std::string(p.label) + " 不能是 0.0.0.0，要填对端的实际地址";
      return false;
    }
  }

  if (s.robot_ip == s.perception_ip) {
    // 官方拓扑里这是两台不同的主机。填成同一个地址时地形图通道会指向
    // 运动主机，上下楼步态会被静默忽略 —— 正是本项目最容易踩的那个坑。
    *error = "运动主机与感知主机不能是同一个地址，上下楼步态需要两条独立通道";
    return false;
  }

  if (!IsIpv4(s.bind_address)) {
    *error = "监听地址不是合法的 IPv4 地址：" + s.bind_address;
    return false;
  }
  // 这一条是整个功能里最要紧的校验。填一个本机没有的地址，重启后 bind 失败，
  // 服务起不来，控制台跟着消失 —— 操作员就再也改不回来了。
  if (s.bind_address != "0.0.0.0" && !IsLocalIpv4(s.bind_address)) {
    *error = "监听地址 " + s.bind_address +
             " 不是本机任何一块网卡的地址。这样重启后服务会起不来，"
             "而且没有控制台可以改回来，所以不允许。";
    return false;
  }

  // 只在**端口**变了的时候试探绑定。
  //
  // 不能在"地址变了"时也试：现在的监听套接字正占着这个端口，而 0.0.0.0 与
  // 任何具体地址在同一端口上是互斥的。于是「把 0.0.0.0 收紧成 192.168.10.2」
  // ——恰恰是文档推荐的加固动作——会被自己占着的端口挡下来，报一个
  // 「端口已被占用」的假错误。端口没变时也不需要试：它本来就在用。
  if (s.http_port != current.http_port) {
    std::string why;
    if (!CanBind(s.bind_address, s.http_port, &why)) {
      *error = "绑不上 " + s.bind_address + ":" + std::to_string(s.http_port) +
               "（" + why + "）。换个端口，或先停掉占用它的程序。";
      return false;
    }
  }

  if (s.local_port == s.http_port) {
    *error = "遥测接收端口与服务端口撞了（都是 " +
             std::to_string(s.http_port) + "）";
    return false;
  }

  if (s.cloud_enabled) {
    if (!ValidRosMaster(s.ros_master, error)) return false;
    if (!IsIpv4(s.ros_host)) {
      *error = "本机在 ROS 网络中的地址不是合法 IPv4：" + s.ros_host;
      return false;
    }
    // 感知主机要靠这个地址反连回来推点云。填成 MESH 侧地址的话订阅会成功、
    // 数据一帧都不来，现场极难查，所以在这里就拦住。
    if (!IsLocalIpv4(s.ros_host)) {
      *error = "本机在 ROS 网络中的地址 " + s.ros_host +
               " 不是本机任何一块网卡的地址。"
               "感知主机要靠它反连回来送点云，填错的症状是订阅成功但收不到数据。";
      return false;
    }
    if (s.cloud_topic.empty() || s.cloud_topic[0] != '/') {
      *error = "点云话题要以 / 开头，例如 /lidar_points";
      return false;
    }
  }

  const std::string* rtsp[] = {&s.ptz_vis_rtsp, &s.ptz_ir_rtsp};
  const char* rtsp_label[] = {"白光 RTSP", "热成像 RTSP"};
  for (int i = 0; i < 2; ++i) {
    if (rtsp[i]->empty()) continue;
    std::string host, user, pass;
    if (!ParseRtspAuthority(*rtsp[i], &host, &user, &pass, error)) {
      *error = std::string(rtsp_label[i]) + " " + *error;
      return false;
    }
  }

  const std::string* codec[] = {&s.ptz_vis_codec, &s.ptz_ir_codec};
  const char* codec_label[] = {"白光编码", "热成像编码"};
  for (int i = 0; i < 2; ++i) {
    if (codec[i]->empty()) continue;
    if (*codec[i] != "h264" && *codec[i] != "h265") {
      *error = std::string(codec_label[i]) + " 只能是 h264 或 h265";
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// 管理令牌
// ---------------------------------------------------------------------------

std::string LoadAdminToken(const std::string& path) {
  if (path.empty()) return "";
  std::ifstream in(path);
  if (!in.is_open()) return "";
  std::string token;
  std::getline(in, token);
  return Trim(token);
}

bool TokenMatches(const std::string& expected, const std::string& given) {
  // 没配令牌就一律不放行，而不是"没配就不校验"。后者在部署里漏一步
  // 就等于把改配置的口子对所有人敞开。
  if (expected.empty()) return false;
  if (expected.size() != given.size()) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < expected.size(); ++i) {
    diff |= static_cast<unsigned char>(expected[i]) ^
            static_cast<unsigned char>(given[i]);
  }
  return diff == 0;
}

// ---------------------------------------------------------------------------
// 布控球 RTSP
// ---------------------------------------------------------------------------

std::string PercentDecode(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (in[i] == '+') {
      out.push_back(' ');
      continue;
    }
    out.push_back(in[i]);
  }
  return out;
}

bool ParseRtspAuthority(const std::string& url, std::string* host,
                        std::string* user, std::string* password,
                        std::string* error) {
  const std::string prefix = "rtsp://";
  if (url.size() < prefix.size() ||
      url.compare(0, prefix.size(), prefix) != 0) {
    if (error) *error = "必须以 rtsp:// 开头";
    return false;
  }
  const std::string rest = url.substr(prefix.size());
  const size_t slash = rest.find('/');
  const size_t auth_end = (slash == std::string::npos) ? rest.size() : slash;
  const std::string ahead = rest.substr(0, auth_end);
  if (ahead.empty()) {
    if (error) *error = "缺少主机";
    return false;
  }

  std::string hostport = ahead;
  const size_t at = ahead.rfind('@');
  if (at != std::string::npos) {
    const std::string cred = ahead.substr(0, at);
    hostport = ahead.substr(at + 1);
    const size_t colon = cred.find(':');
    const std::string raw_user =
        (colon == std::string::npos) ? cred : cred.substr(0, colon);
    const std::string raw_pass =
        (colon == std::string::npos) ? "" : cred.substr(colon + 1);
    // RTSP 地址里口令常写成 p%40ss，MediaMTX 会解码，ISAPI 必须解成 p@ss。
    if (user) *user = PercentDecode(raw_user);
    if (password) *password = PercentDecode(raw_pass);
  } else {
    if (user) user->clear();
    if (password) password->clear();
  }

  if (hostport.empty()) {
    if (error) *error = "缺少主机";
    return false;
  }
  const size_t colon = hostport.rfind(':');
  std::string h = hostport;
  if (colon != std::string::npos && colon > 0) h = hostport.substr(0, colon);
  if (host) *host = h;
  return true;
}

std::string MediamtxPathBeside(const std::string& media_json) {
  if (media_json.empty()) return "";
  const size_t slash = media_json.find_last_of('/');
  if (slash == std::string::npos) return "mediamtx.yml";
  return media_json.substr(0, slash + 1) + "mediamtx.yml";
}

std::string DeriveHikSubRtsp(const std::string& main_url) {
  if (main_url.size() < 3) return "";
  const std::string tail = main_url.substr(main_url.size() - 3);
  if (tail == "101") return main_url.substr(0, main_url.size() - 3) + "102";
  if (tail == "201") return main_url.substr(0, main_url.size() - 3) + "202";
  return "";
}

std::string ReadMediamtxSource(const std::string& yml_path,
                               const std::string& path_name) {
  std::ifstream in(yml_path);
  if (!in) return "";
  std::string line;
  std::string current;
  const std::string want = path_name + ":";
  while (std::getline(in, line)) {
    std::string t = Trim(line);
    if (!t.empty() && t.back() == ':' && t.find(' ') == std::string::npos) {
      current = t;
    }
    if (current != want) continue;
    const std::string key = "source:";
    const size_t pos = t.find(key);
    if (pos != 0) continue;
    return Trim(t.substr(key.size()));
  }
  return "";
}

bool PatchMediamtxSource(const std::string& yml_path,
                         const std::string& path_name,
                         const std::string& source, std::string* error) {
  std::ifstream in(yml_path);
  if (!in) {
    if (error) *error = "打不开 " + yml_path;
    return false;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);

  std::string current;
  const std::string want = path_name + ":";
  bool found = false;
  for (auto& raw : lines) {
    const std::string t = Trim(raw);
    if (!t.empty() && t.back() == ':' && t.find(' ') == std::string::npos) {
      current = t;
    }
    if (current != want) continue;
    if (t.compare(0, 7, "source:") != 0) continue;
    const size_t indent = raw.find_first_not_of(" \t");
    raw = (indent == std::string::npos ? std::string() : raw.substr(0, indent)) +
          "source: " + source;
    found = true;
    break;
  }
  if (!found) {
    if (error) *error = yml_path + " 里没有 " + path_name + " 的 source";
    return false;
  }

  const std::string tmp = yml_path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  bool atomic = f != nullptr;
  if (f == nullptr) {
    // ProtectSystem=strict 时 /opt/x30 目录不可写，.tmp 建不出来。
    // 已经存在的 yml 若在 ReadWritePaths 里，可以就地覆盖。
    f = std::fopen(yml_path.c_str(), "w");
  }
  if (f == nullptr) {
    if (error) *error = "写不了 " + yml_path;
    return false;
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    std::fprintf(f, "%s\n", lines[i].c_str());
  }
  std::fclose(f);
  if (atomic && ::rename(tmp.c_str(), yml_path.c_str()) != 0) {
    if (error) *error = "替换 " + yml_path + " 失败";
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::string EscapeRtspJson(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

bool PatchMediamtxSourceApi(const std::string& path_name,
                            const std::string& source) {
  char cmd[2048];
  const int n = std::snprintf(
      cmd, sizeof(cmd),
      "curl -sS --connect-timeout 0.5 --max-time 2 "
      "-X PATCH -H 'Content-Type: application/json' "
      "-d '{\"source\":\"%s\"}' "
      "'http://127.0.0.1:9997/v3/config/paths/patch/%s' >/dev/null 2>&1",
      EscapeRtspJson(source).c_str(), path_name.c_str());
  if (n < 0 || static_cast<size_t>(n) >= sizeof(cmd)) return false;
  return std::system(cmd) == 0;
}

bool ApplyPtzRtspToMediamtx(const std::string& yml_path,
                            const GatewaySettings& s, std::string* error,
                            bool* wrote) {
  if (wrote) *wrote = false;
  if (yml_path.empty()) return true;
  {
    std::ifstream probe(yml_path);
    if (!probe) return true;
  }
  struct Item {
    const char* main;
    const char* sub;
    const std::string& url;
  };
  const Item items[] = {{"ptz_vis_main", "ptz_vis_sub", s.ptz_vis_rtsp},
                        {"ptz_ir_main", "ptz_ir_sub", s.ptz_ir_rtsp}};
  for (const auto& it : items) {
    if (it.url.empty()) continue;
    std::string sub = DeriveHikSubRtsp(it.url);
    if (sub.empty()) sub = it.url;
    const bool file_same = ReadMediamtxSource(yml_path, it.main) == it.url &&
                           ReadMediamtxSource(yml_path, it.sub) == sub;

    std::string file_err;
    bool file_ok = file_same;
    if (!file_same) {
      file_ok = PatchMediamtxSource(yml_path, it.main, it.url, &file_err);
      if (file_ok) {
        std::string ignore;
        PatchMediamtxSource(yml_path, it.sub, sub, &ignore);
      }
    }

    const bool api_ok = PatchMediamtxSourceApi(it.main, it.url) &&
                        PatchMediamtxSourceApi(it.sub, sub);
    if (!file_ok && !api_ok) {
      if (error) *error = file_err.empty() ? "MediaMTX 拉流地址没写上" : file_err;
      return false;
    }
    // 文件写上了但 API 没通，才需要重启 MediaMTX 去读文件。
    // API 已经改过的话重启反而会用旧文件把地址冲掉。
    if (wrote && file_ok && !file_same && !api_ok) *wrote = true;
  }
  return true;
}

std::string InferRtspCodec(const std::string& url) {
  std::string rest = url;
  if (rest.size() >= 7 && rest.compare(0, 7, "rtsp://") == 0) rest = rest.substr(7);
  const size_t slash = rest.find('/');
  if (slash == std::string::npos) return "";
  std::string path = rest.substr(slash);
  for (char& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (path.find("h265") != std::string::npos ||
      path.find("hevc") != std::string::npos) {
    return "h265";
  }
  if (path.find("h264") != std::string::npos) return "h264";
  return "";
}

std::string EffectivePtzCodec(const std::string& configured,
                              const std::string& rtsp_url) {
  if (!configured.empty()) return configured;
  return InferRtspCodec(rtsp_url);
}

}  // namespace x30
