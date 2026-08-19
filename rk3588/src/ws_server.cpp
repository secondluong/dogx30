#include "x30/ws_server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCKET closesocket
using ssize_t_compat = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET close
using ssize_t_compat = ssize_t;
#endif

namespace x30 {
namespace {

// --- SHA-1（RFC 3174）。仅用于 WebSocket 握手，不作通用密码学用途 -----------

class Sha1 {
 public:
  void Update(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      buffer_[buffer_len_++] = data[i];
      if (buffer_len_ == 64) {
        Transform(buffer_);
        buffer_len_ = 0;
        total_ += 64;
      }
    }
  }

  void Final(uint8_t out[20]) {
    const uint64_t bits = (total_ + buffer_len_) * 8;
    uint8_t pad = 0x80;
    Update(&pad, 1);
    pad = 0x00;
    while (buffer_len_ != 56) Update(&pad, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) len_be[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    Update(len_be, 8);
    for (int i = 0; i < 5; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
    }
  }

 private:
  static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

  void Transform(const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t tmp = Rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = Rol(b, 30);
      b = a;
      a = tmp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
  }

  uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t buffer_[64] = {};
  size_t buffer_len_ = 0;
  uint64_t total_ = 0;
};

std::string Base64(const uint8_t* data, size_t len) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t a = data[i];
    const uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out += kAlphabet[(triple >> 18) & 0x3F];
    out += kAlphabet[(triple >> 12) & 0x3F];
    out += (i + 1 < len) ? kAlphabet[(triple >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? kAlphabet[triple & 0x3F] : '=';
  }
  return out;
}

std::string AcceptKey(const std::string& client_key) {
  static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const std::string combined = client_key + kGuid;
  Sha1 sha;
  sha.Update(reinterpret_cast<const uint8_t*>(combined.data()), combined.size());
  uint8_t digest[20];
  sha.Final(digest);
  return Base64(digest, sizeof(digest));
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return s;
}

std::string Trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string MimeType(const std::string& path) {
  const auto dot = path.find_last_of('.');
  const std::string ext = dot == std::string::npos ? "" : ToLower(path.substr(dot));
  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  if (ext == ".js") return "application/javascript; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".json") return "application/json; charset=utf-8";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".ico") return "image/x-icon";
  return "application/octet-stream";
}

// 只允许访问静态根目录下的文件。拒绝任何含 ".." 或以 "/" 开头的相对片段，
// 避免通过 ../../etc/passwd 这类路径穿越读到根目录外。
bool IsSafeRelativePath(const std::string& path) {
  if (path.find("..") != std::string::npos) return false;
  if (path.find('\\') != std::string::npos) return false;
  if (path.find('\0') != std::string::npos) return false;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------

struct WsServer::Connection {
  WsServer::ClientId id = 0;
  int fd = -1;
  std::atomic<bool> open{false};
  std::mutex send_mutex;
  std::string inbox;  // 分片重组缓冲

  bool SendRaw(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(send_mutex);
    const char* p = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
      const ssize_t_compat n =
          ::send(fd, p, static_cast<int>(remaining), 0);
      if (n <= 0) return false;
      p += n;
      remaining -= static_cast<size_t>(n);
    }
    return true;
  }

  // 服务端发出的帧不掩码（RFC 6455 §5.1）。
  bool SendFrame(uint8_t opcode, const std::string& payload) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    const size_t n = payload.size();
    if (n < 126) {
      frame += static_cast<char>(n);
    } else if (n <= 0xFFFF) {
      frame += static_cast<char>(126);
      frame += static_cast<char>((n >> 8) & 0xFF);
      frame += static_cast<char>(n & 0xFF);
    } else {
      frame += static_cast<char>(127);
      for (int i = 7; i >= 0; --i) {
        frame += static_cast<char>((static_cast<uint64_t>(n) >> (i * 8)) & 0xFF);
      }
    }
    frame += payload;
    return SendRaw(frame.data(), frame.size());
  }
};

WsServer::WsServer() = default;

WsServer::~WsServer() { Stop(); }

void WsServer::SetHandlers(ConnectHandler on_connect, MessageHandler on_message,
                           DisconnectHandler on_disconnect) {
  on_connect_ = std::move(on_connect);
  on_message_ = std::move(on_message);
  on_disconnect_ = std::move(on_disconnect);
}

bool WsServer::Start(const std::string& bind_address, uint16_t port,
                     std::string* error) {
#if defined(_WIN32)
  {
    static bool wsa_started = false;
    if (!wsa_started) {
      WSADATA data;
      WSAStartup(MAKEWORD(2, 2), &data);
      wsa_started = true;
    }
  }
#endif

  listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (listen_fd_ < 0) {
    if (error) *error = "创建监听套接字失败";
    return false;
  }

  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

#if defined(IP_FREEBIND)
  // 允许绑定到尚未配起来的地址。开机时网关往往比 hostapd 先就绪，
  // 没有这一项就会因为热点地址还不存在而启动失败。
  int freebind = 1;
  ::setsockopt(listen_fd_, IPPROTO_IP, IP_FREEBIND, &freebind, sizeof(freebind));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (bind_address.empty() || bind_address == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
    if (error) *error = "监听地址无法解析: " + bind_address;
    CLOSE_SOCKET(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (error) *error = "绑定端口 " + std::to_string(port) + " 失败（可能已被占用）";
    CLOSE_SOCKET(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 8) != 0) {
    if (error) *error = "listen 失败";
    CLOSE_SOCKET(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  port_ = port;
  running_.store(true);
  accept_thread_ = std::thread(&WsServer::AcceptLoop, this);
  return true;
}

void WsServer::Stop() {
  if (!running_.exchange(false)) return;

  if (listen_fd_ >= 0) {
    CLOSE_SOCKET(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();

  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& [id, conn] : clients_) {
      conn->open.store(false);
      if (conn->fd >= 0) {
#if defined(_WIN32)
        ::shutdown(conn->fd, SD_BOTH);
#else
        ::shutdown(conn->fd, SHUT_RDWR);
#endif
      }
    }
  }
  for (auto& t : session_threads_) {
    if (t.joinable()) t.join();
  }
  session_threads_.clear();
  clients_.clear();
}

void WsServer::AcceptLoop() {
  while (running_.load()) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listen_fd_, &read_set);
    timeval tv{0, 200000};
    const int ready = ::select(listen_fd_ + 1, &read_set, nullptr, nullptr, &tv);
    if (ready <= 0) continue;
    if (!running_.load()) break;

    const int fd = static_cast<int>(::accept(listen_fd_, nullptr, nullptr));
    if (fd < 0) continue;

    // 控制指令是小包高频，Nagle 会把它们攒起来，直接拉高遥控延迟。
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    // 发送超时。没有它，一个读得慢的客户端会把 send() 卡死，而 send() 持有
    // 该连接的发送锁 —— 后果是 10 Hz 遥测广播跟着卡住，所有人的界面一起冻。
    // 点云帧有上百 KB，在拥塞的 MESH 上这不是理论风险。
    // 超时后 SendRaw 返回 false，上层据此丢帧而不是无限等待。
#if defined(_WIN32)
    DWORD send_timeout = 2000;
#else
    timeval send_timeout{};
    send_timeout.tv_sec = 2;
#endif
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&send_timeout),
                 sizeof(send_timeout));

    auto conn = std::make_shared<Connection>();
    conn->id = next_id_.fetch_add(1);
    conn->fd = fd;

    // 回收已结束的会话线程，避免长时间运行后 vector 无限增长。
    session_threads_.erase(
        std::remove_if(session_threads_.begin(), session_threads_.end(),
                       [](std::thread& t) {
                         if (!t.joinable()) return true;
                         return false;
                       }),
        session_threads_.end());

    session_threads_.emplace_back(&WsServer::SessionLoop, this, conn);
  }
}

bool WsServer::ServeHttp(Connection& conn, const std::string& method,
                         const std::string& path) {
  auto respond = [&](const std::string& status, const std::string& type,
                     const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    const std::string out = oss.str();
    conn.SendRaw(out.data(), out.size());
  };

  if (method != "GET") {
    respond("405 Method Not Allowed", "text/plain; charset=utf-8", "只支持 GET");
    return false;
  }
  if (static_root_.empty()) {
    respond("404 Not Found", "text/plain; charset=utf-8", "未配置静态目录");
    return false;
  }

  // 必须先去掉查询串。App 打开 /?shell=app 时，请求行里的 path 不是 "/"，
  // 若先按整段判断，会落到空文件名，登录后就是一张白屏。
  std::string clean = path;
  const auto query = clean.find('?');
  if (query != std::string::npos) clean = clean.substr(0, query);
  if (clean.empty() || clean == "/") clean = "/index.html";
  std::string rel = clean.front() == '/' ? clean.substr(1) : clean;
  if (!IsSafeRelativePath(rel)) {
    respond("403 Forbidden", "text/plain; charset=utf-8", "非法路径");
    return false;
  }

  std::ifstream file(static_root_ + "/" + rel, std::ios::binary);
  if (!file) {
    respond("404 Not Found", "text/plain; charset=utf-8", "文件不存在: " + rel);
    return false;
  }
  std::ostringstream body;
  body << file.rdbuf();
  respond("200 OK", MimeType(rel), body.str());
  return false;  // HTTP 请求处理完即关闭连接
}

void WsServer::SessionLoop(std::shared_ptr<Connection> conn) {
  // --- 读取 HTTP 请求头 ---
  std::string request;
  char buf[4096];
  bool header_done = false;
  while (running_.load() && request.size() < 16384) {
    const ssize_t_compat n = ::recv(conn->fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    request.append(buf, static_cast<size_t>(n));
    if (request.find("\r\n\r\n") != std::string::npos) {
      header_done = true;
      break;
    }
  }

  if (!header_done) {
    CLOSE_SOCKET(conn->fd);
    return;
  }

  std::istringstream stream(request);
  std::string line;
  std::getline(stream, line);
  std::istringstream request_line(line);
  std::string method, path, version;
  request_line >> method >> path >> version;

  std::map<std::string, std::string> headers;
  while (std::getline(stream, line) && line != "\r" && !line.empty()) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
  }

  const bool wants_ws =
      ToLower(headers["upgrade"]).find("websocket") != std::string::npos &&
      headers.count("sec-websocket-key") > 0;

  if (!wants_ws) {
    ServeHttp(*conn, method, path);
    CLOSE_SOCKET(conn->fd);
    return;
  }

  // --- WebSocket 握手 ---
  const std::string accept = AcceptKey(headers["sec-websocket-key"]);
  std::ostringstream handshake;
  handshake << "HTTP/1.1 101 Switching Protocols\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
  const std::string hs = handshake.str();
  if (!conn->SendRaw(hs.data(), hs.size())) {
    CLOSE_SOCKET(conn->fd);
    return;
  }

  conn->open.store(true);
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_[conn->id] = conn;
  }
  if (on_connect_) on_connect_(conn->id);

  // --- 帧循环 ---
  std::string rx;
  std::string fragment;
  uint8_t fragment_opcode = 0;

  while (running_.load() && conn->open.load()) {
    const ssize_t_compat n = ::recv(conn->fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    rx.append(buf, static_cast<size_t>(n));

    // 一个 TCP 段里可能有多帧，也可能半帧，循环直到攒不出完整帧为止。
    while (true) {
      if (rx.size() < 2) break;
      const auto b0 = static_cast<uint8_t>(rx[0]);
      const auto b1 = static_cast<uint8_t>(rx[1]);
      const bool fin = (b0 & 0x80) != 0;
      const uint8_t opcode = b0 & 0x0F;
      const bool masked = (b1 & 0x80) != 0;
      uint64_t len = b1 & 0x7F;
      size_t offset = 2;

      if (len == 126) {
        if (rx.size() < offset + 2) break;
        len = (static_cast<uint64_t>(static_cast<uint8_t>(rx[offset])) << 8) |
              static_cast<uint8_t>(rx[offset + 1]);
        offset += 2;
      } else if (len == 127) {
        if (rx.size() < offset + 8) break;
        len = 0;
        for (int i = 0; i < 8; ++i) {
          len = (len << 8) | static_cast<uint8_t>(rx[offset + i]);
        }
        offset += 8;
      }

      // 客户端必须掩码；同时挡住异常大的帧，避免被一个坏包撑爆内存。
      if (!masked || len > (8u << 20)) {
        conn->open.store(false);
        break;
      }
      if (rx.size() < offset + 4) break;
      uint8_t mask[4];
      std::memcpy(mask, rx.data() + offset, 4);
      offset += 4;
      if (rx.size() < offset + len) break;

      std::string payload(rx, offset, static_cast<size_t>(len));
      for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
      }
      rx.erase(0, offset + static_cast<size_t>(len));

      switch (opcode) {
        case 0x0:  // 续帧
          fragment += payload;
          if (fin && on_message_ && fragment_opcode == 0x1) {
            on_message_(conn->id, fragment);
          }
          if (fin) {
            fragment.clear();
            fragment_opcode = 0;
          }
          break;
        case 0x1:  // 文本
        case 0x2:  // 二进制
          if (fin) {
            if (opcode == 0x1 && on_message_) on_message_(conn->id, payload);
          } else {
            fragment = payload;
            fragment_opcode = opcode;
          }
          break;
        case 0x8:  // close
          conn->SendFrame(0x8, "");
          conn->open.store(false);
          break;
        case 0x9:  // ping -> pong
          conn->SendFrame(0xA, payload);
          break;
        case 0xA:  // pong，忽略
          break;
        default:
          conn->open.store(false);
          break;
      }
      if (!conn->open.load()) break;
    }
  }

  conn->open.store(false);
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.erase(conn->id);
  }
  if (on_disconnect_) on_disconnect_(conn->id);
  CLOSE_SOCKET(conn->fd);
  conn->fd = -1;
}

void WsServer::Send(ClientId id, const std::string& text) {
  std::shared_ptr<Connection> conn;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    const auto it = clients_.find(id);
    if (it == clients_.end()) return;
    conn = it->second;
  }
  if (conn->open.load()) conn->SendFrame(0x1, text);
}

bool WsServer::SendBinary(ClientId id, const void* data, size_t len) {
  std::shared_ptr<Connection> conn;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    const auto it = clients_.find(id);
    if (it == clients_.end()) return false;
    conn = it->second;
  }
  if (!conn->open.load()) return false;
  return conn->SendFrame(
      0x2, std::string(static_cast<const char*>(data), len));
}

void WsServer::Broadcast(const std::string& text) {
  std::vector<std::shared_ptr<Connection>> targets;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    targets.reserve(clients_.size());
    for (auto& [id, conn] : clients_) targets.push_back(conn);
  }
  for (auto& conn : targets) {
    if (conn->open.load()) conn->SendFrame(0x1, text);
  }
}

size_t WsServer::ClientCount() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  return clients_.size();
}

std::vector<WsServer::ClientId> WsServer::ClientIds() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  std::vector<ClientId> ids;
  ids.reserve(clients_.size());
  for (const auto& [id, conn] : clients_) ids.push_back(id);
  return ids;
}

}  // namespace x30
