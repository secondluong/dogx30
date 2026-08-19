#include "x30/xmlrpc.hpp"

#include "x30/net_util.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t_compat = int;
using ssize_t_compat = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_t_compat = socklen_t;
using ssize_t_compat = ssize_t;
#define closesocket ::close
#endif

namespace x30 {
namespace {

const XmlRpcValue& NullValue() {
  static const XmlRpcValue v;
  return v;
}

std::string XmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string XmlUnescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 4; continue; }
      if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 4; continue; }
      if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; continue; }
      if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; continue; }
      if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; continue; }
    }
    out += s[i];
    ++i;
  }
  return out;
}

// 从 pos 起找下一个 <tag>，返回标签名与其内容范围。这是个极简的顺序扫描器，
// 不做校验 —— 对手是 ROS master，不是不可信输入。
bool NextTag(const std::string& xml, size_t pos, std::string* name,
             size_t* content_begin, size_t* tag_end) {
  const size_t lt = xml.find('<', pos);
  if (lt == std::string::npos) return false;
  const size_t gt = xml.find('>', lt);
  if (gt == std::string::npos) return false;
  *name = xml.substr(lt + 1, gt - lt - 1);
  *content_begin = gt + 1;
  *tag_end = gt + 1;
  return true;
}

// 解析一个 <value> 元素的内容（不含 <value> 标签本身）。
// begin 指向 <value> 之后，返回解析出的值并把 end 指到对应 </value> 之后。
XmlRpcValue ParseValue(const std::string& xml, size_t begin, size_t* end);

XmlRpcValue ParseArray(const std::string& xml, size_t begin, size_t* end) {
  XmlRpcValue v;
  v.type = XmlRpcValue::Type::kArray;
  size_t pos = xml.find("<data>", begin);
  if (pos == std::string::npos) {
    *end = begin;
    return v;
  }
  pos += 6;
  while (true) {
    const size_t next_value = xml.find("<value>", pos);
    const size_t data_end = xml.find("</data>", pos);
    if (next_value == std::string::npos || data_end == std::string::npos ||
        next_value > data_end) {
      break;
    }
    size_t item_end = 0;
    v.array.push_back(ParseValue(xml, next_value + 7, &item_end));
    pos = item_end;
  }
  const size_t data_end = xml.find("</data>", pos);
  *end = data_end == std::string::npos ? pos : data_end + 7;
  return v;
}

XmlRpcValue ParseValue(const std::string& xml, size_t begin, size_t* end) {
  XmlRpcValue v;

  // <value> 里可能直接是文本（等价于 string），也可能包一层类型标签。
  size_t p = begin;
  while (p < xml.size() && (xml[p] == ' ' || xml[p] == '\n' || xml[p] == '\r' ||
                            xml[p] == '\t')) {
    ++p;
  }
  if (p >= xml.size()) {
    *end = begin;
    return v;
  }

  if (xml[p] != '<') {
    const size_t close = xml.find("</value>", p);
    v.type = XmlRpcValue::Type::kString;
    v.str_value = XmlUnescape(xml.substr(p, close - p));
    *end = close == std::string::npos ? p : close + 8;
    return v;
  }

  std::string tag;
  size_t content = 0, tag_end = 0;
  if (!NextTag(xml, p, &tag, &content, &tag_end)) {
    *end = begin;
    return v;
  }

  // 空字符串常见写法是 <value></value>。begin 此时正指着闭合标签，
  // 若当普通类型往下扫，会吞掉后面整个数组——requestTopic 的
  // [1, '', ['TCPROS', host, port]] 就会变成「TCPROS 地址无效」。
  if (!tag.empty() && tag[0] == '/') {
    v.type = XmlRpcValue::Type::kString;
    *end = tag_end;
    return v;
  }

  if (tag == "array") {
    size_t arr_end = 0;
    v = ParseArray(xml, content, &arr_end);
    const size_t close = xml.find("</value>", arr_end);
    *end = close == std::string::npos ? arr_end : close + 8;
    return v;
  }

  const std::string close_tag = "</" + tag + ">";
  const size_t close = xml.find(close_tag, content);
  const std::string text =
      close == std::string::npos ? "" : xml.substr(content, close - content);

  if (tag == "int" || tag == "i4") {
    v.type = XmlRpcValue::Type::kInt;
    v.int_value = std::atoi(text.c_str());
  } else if (tag == "boolean") {
    v.type = XmlRpcValue::Type::kInt;
    v.int_value = std::atoi(text.c_str());
  } else {
    v.type = XmlRpcValue::Type::kString;
    v.str_value = XmlUnescape(text);
  }

  const size_t after = close == std::string::npos ? content
                                                  : close + close_tag.size();
  const size_t value_close = xml.find("</value>", after);
  *end = value_close == std::string::npos ? after : value_close + 8;
  return v;
}

bool SplitUri(const std::string& uri, std::string* host, uint16_t* port,
              std::string* path) {
  const std::string prefix = "http://";
  if (uri.compare(0, prefix.size(), prefix) != 0) return false;
  const size_t host_begin = prefix.size();
  const size_t slash = uri.find('/', host_begin);
  const std::string authority =
      uri.substr(host_begin, slash == std::string::npos
                                 ? std::string::npos
                                 : slash - host_begin);
  *path = slash == std::string::npos ? "/" : uri.substr(slash);

  const size_t colon = authority.rfind(':');
  if (colon == std::string::npos) {
    *host = authority;
    *port = 80;
  } else {
    *host = authority.substr(0, colon);
    *port = static_cast<uint16_t>(std::atoi(authority.c_str() + colon + 1));
  }
  return !host->empty();
}

bool SendAll(int fd, const char* data, size_t len) {
  while (len > 0) {
    const ssize_t_compat n = ::send(fd, data, static_cast<int>(len), 0);
    if (n <= 0) return false;
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

void SetTimeouts(int fd, int ms) {
#if defined(_WIN32)
  DWORD tv = static_cast<DWORD>(ms);
#else
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
#endif
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
}

}  // namespace

// ---------------------------------------------------------------------------

XmlRpcValue XmlRpcValue::Int(int v) {
  XmlRpcValue x;
  x.type = Type::kInt;
  x.int_value = v;
  return x;
}

XmlRpcValue XmlRpcValue::Str(std::string v) {
  XmlRpcValue x;
  x.type = Type::kString;
  x.str_value = std::move(v);
  return x;
}

XmlRpcValue XmlRpcValue::Array(std::vector<XmlRpcValue> v) {
  XmlRpcValue x;
  x.type = Type::kArray;
  x.array = std::move(v);
  return x;
}

const XmlRpcValue& XmlRpcValue::At(size_t i) const {
  if (type != Type::kArray || i >= array.size()) return NullValue();
  return array[i];
}

std::string XmlRpcValue::ToXml() const {
  std::string out = "<value>";
  switch (type) {
    case Type::kInt:
      out += "<int>" + std::to_string(int_value) + "</int>";
      break;
    case Type::kString:
      out += "<string>" + XmlEscape(str_value) + "</string>";
      break;
    case Type::kArray:
      out += "<array><data>";
      for (const auto& item : array) out += item.ToXml();
      out += "</data></array>";
      break;
  }
  out += "</value>";
  return out;
}

bool ParseMethodCall(const std::string& xml, std::string* method,
                     std::vector<XmlRpcValue>* params) {
  const size_t name_begin = xml.find("<methodName>");
  if (name_begin == std::string::npos) return false;
  const size_t name_end = xml.find("</methodName>", name_begin);
  if (name_end == std::string::npos) return false;
  *method = xml.substr(name_begin + 12, name_end - name_begin - 12);

  params->clear();
  size_t pos = name_end;
  while (true) {
    const size_t v = xml.find("<value>", pos);
    if (v == std::string::npos) break;
    size_t end = 0;
    params->push_back(ParseValue(xml, v + 7, &end));
    pos = end;
  }
  return true;
}

bool XmlRpcCall(const std::string& uri, const std::string& method,
                const std::vector<XmlRpcValue>& params, XmlRpcValue* result,
                std::string* error, int timeout_ms) {
  std::string host, path;
  uint16_t port = 0;
  if (!SplitUri(uri, &host, &port, &path)) {
    if (error) *error = "无法解析 URI: " + uri;
    return false;
  }

  std::string body = "<?xml version=\"1.0\"?><methodCall><methodName>" +
                     method + "</methodName><params>";
  for (const auto& p : params) body += "<param>" + p.ToXml() + "</param>";
  body += "</params></methodCall>";

  std::string request = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
  request += "Content-Type: text/xml\r\n";
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += body;

  const int fd = TcpConnectTimeout(host, port, timeout_ms, timeout_ms);
  if (fd < 0) {
    if (error) *error = "连接 " + uri + " 失败";
    return false;
  }

  if (!SendAll(fd, request.data(), request.size())) {
    closesocket(fd);
    if (error) *error = "发送请求失败";
    return false;
  }

  std::string response;
  char buf[4096];
  while (true) {
    const ssize_t_compat n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    response.append(buf, static_cast<size_t>(n));
    // 简化处理：依赖 Connection: close，读到对端关闭为止。
    if (response.size() > 4u * 1024 * 1024) break;
  }
  closesocket(fd);

  const size_t body_begin = response.find("\r\n\r\n");
  if (body_begin == std::string::npos) {
    if (error) *error = "响应不完整";
    return false;
  }

  const size_t fault = response.find("<fault>", body_begin);
  if (fault != std::string::npos) {
    if (error) *error = "对端返回 fault";
    return false;
  }

  const size_t v = response.find("<value>", body_begin);
  if (v == std::string::npos) {
    if (error) *error = "响应里没有 value";
    return false;
  }
  size_t end = 0;
  *result = ParseValue(response, v + 7, &end);
  return true;
}

// ---------------------------------------------------------------------------

struct XmlRpcServer::Impl {
  int listen_fd = -1;
  std::atomic<bool> running{false};
  std::thread accept_thread;
  std::mutex handlers_mutex;
  std::map<std::string, Handler> handlers;

  void Serve(int fd) {
    SetTimeouts(fd, 3000);
    std::string request;
    char buf[4096];
    size_t content_length = 0;
    size_t header_end = std::string::npos;

    while (true) {
      const ssize_t_compat n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      request.append(buf, static_cast<size_t>(n));

      if (header_end == std::string::npos) {
        header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos) {
          const size_t cl = request.find("Content-Length:");
          if (cl != std::string::npos && cl < header_end) {
            content_length = static_cast<size_t>(
                std::atoi(request.c_str() + cl + 15));
          }
        }
      }
      if (header_end != std::string::npos &&
          request.size() >= header_end + 4 + content_length) {
        break;
      }
      if (request.size() > 1024u * 1024) break;
    }

    XmlRpcValue result = XmlRpcValue::Array(
        {XmlRpcValue::Int(1), XmlRpcValue::Str("ok"), XmlRpcValue::Int(0)});

    if (header_end != std::string::npos) {
      const std::string body = request.substr(header_end + 4);
      std::string method;
      std::vector<XmlRpcValue> params;
      if (ParseMethodCall(body, &method, &params)) {
        Handler h;
        {
          std::lock_guard<std::mutex> lock(handlers_mutex);
          const auto it = handlers.find(method);
          if (it != handlers.end()) h = it->second;
        }
        if (h) result = h(params);
      }
    }

    const std::string body =
        "<?xml version=\"1.0\"?><methodResponse><params><param>" +
        result.ToXml() + "</param></params></methodResponse>";
    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/xml\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    SendAll(fd, response.data(), response.size());
    closesocket(fd);
  }

  void AcceptLoop() {
    while (running.load()) {
      // 必须先 select 再 accept。直接阻塞在 accept 上的话，Stop() 里关掉监听
      // 套接字并不能可靠唤醒它（Linux 上不保证），join 就会永远等下去。
      fd_set read_set;
      FD_ZERO(&read_set);
      FD_SET(listen_fd, &read_set);
      timeval tv{0, 200000};
      const int ready = ::select(listen_fd + 1, &read_set, nullptr, nullptr,
                                 &tv);
      if (ready <= 0) continue;
      if (!running.load()) break;

      sockaddr_in peer{};
      socklen_t_compat len = sizeof(peer);
      const int fd = static_cast<int>(
          ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len));
      if (fd < 0) {
        if (!running.load()) break;
        continue;
      }
      // 每个请求都很短，就地处理，不额外起线程。ROS master 的回调频率很低。
      Serve(fd);
    }
  }
};

XmlRpcServer::XmlRpcServer() : impl_(new Impl) {}

XmlRpcServer::~XmlRpcServer() { Stop(); }

void XmlRpcServer::On(const std::string& method, Handler handler) {
  std::lock_guard<std::mutex> lock(impl_->handlers_mutex);
  impl_->handlers[method] = std::move(handler);
}

bool XmlRpcServer::Start(const std::string& bind_address, uint16_t port,
                         std::string* error) {
  if (impl_->running.load()) return true;

  impl_->listen_fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (impl_->listen_fd < 0) {
    if (error) *error = "创建监听 socket 失败";
    return false;
  }
  int reuse = 1;
  ::setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (bind_address.empty() || bind_address == "0.0.0.0") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
    closesocket(impl_->listen_fd);
    impl_->listen_fd = -1;
    if (error) *error = "无效的监听地址: " + bind_address;
    return false;
  }

  if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
    closesocket(impl_->listen_fd);
    impl_->listen_fd = -1;
    if (error) *error = "绑定端口失败";
    return false;
  }
  if (::listen(impl_->listen_fd, 8) != 0) {
    closesocket(impl_->listen_fd);
    impl_->listen_fd = -1;
    if (error) *error = "listen 失败";
    return false;
  }

  sockaddr_in actual{};
  socklen_t_compat alen = sizeof(actual);
  if (::getsockname(impl_->listen_fd, reinterpret_cast<sockaddr*>(&actual),
                    &alen) == 0) {
    port_ = ntohs(actual.sin_port);
  }

  impl_->running.store(true);
  impl_->accept_thread = std::thread(&Impl::AcceptLoop, impl_.get());
  return true;
}

void XmlRpcServer::Stop() {
  if (!impl_->running.exchange(false)) return;
  if (impl_->listen_fd >= 0) {
    closesocket(impl_->listen_fd);
    impl_->listen_fd = -1;
  }
  if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
}

}  // namespace x30
