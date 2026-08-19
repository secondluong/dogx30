#include "x30/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace x30 {
namespace {
const Json& NullJson() {
  static const Json kNull;
  return kNull;
}
}  // namespace

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text) {}

  bool ParseValue(Json* out) {
    SkipSpace();
    if (pos_ >= s_.size()) return false;
    switch (s_[pos_]) {
      case '{':
        return ParseObject(out);
      case '[':
        return ParseArray(out);
      case '"':
        return ParseString(out);
      case 't':
      case 'f':
        return ParseBool(out);
      case 'n':
        return ParseNull(out);
      default:
        return ParseNumber(out);
    }
  }

 private:
  void SkipSpace() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
            s_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Expect(char c) {
    SkipSpace();
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool ParseObject(Json* out) {
    if (!Expect('{')) return false;
    out->type_ = Json::Type::kObject;
    SkipSpace();
    if (Expect('}')) return true;
    while (true) {
      Json key;
      SkipSpace();
      if (!ParseString(&key)) return false;
      if (!Expect(':')) return false;
      Json value;
      if (!ParseValue(&value)) return false;
      out->object_[key.string_] = std::move(value);
      SkipSpace();
      if (Expect(',')) continue;
      return Expect('}');
    }
  }

  bool ParseArray(Json* out) {
    if (!Expect('[')) return false;
    out->type_ = Json::Type::kArray;
    SkipSpace();
    if (Expect(']')) return true;
    while (true) {
      Json value;
      if (!ParseValue(&value)) return false;
      out->array_.push_back(std::move(value));
      SkipSpace();
      if (Expect(',')) continue;
      return Expect(']');
    }
  }

  bool ParseString(Json* out) {
    if (!Expect('"')) return false;
    out->type_ = Json::Type::kString;
    std::string result;
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') {
        out->string_ = std::move(result);
        return true;
      }
      if (c != '\\') {
        result += c;
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char esc = s_[pos_++];
      switch (esc) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          if (pos_ + 4 > s_.size()) return false;
          const std::string hex = s_.substr(pos_, 4);
          pos_ += 4;
          const auto cp =
              static_cast<unsigned>(std::strtoul(hex.c_str(), nullptr, 16));
          // 只处理 BMP，代理对不在协议范围内。按 UTF-8 编码回去。
          if (cp < 0x80) {
            result += static_cast<char>(cp);
          } else if (cp < 0x800) {
            result += static_cast<char>(0xC0 | (cp >> 6));
            result += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            result += static_cast<char>(0xE0 | (cp >> 12));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
          }
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool ParseBool(Json* out) {
    if (s_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      out->type_ = Json::Type::kBool;
      out->bool_ = true;
      return true;
    }
    if (s_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      out->type_ = Json::Type::kBool;
      out->bool_ = false;
      return true;
    }
    return false;
  }

  bool ParseNull(Json* out) {
    if (s_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      out->type_ = Json::Type::kNull;
      return true;
    }
    return false;
  }

  bool ParseNumber(Json* out) {
    const size_t start = pos_;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
    bool any = false;
    while (pos_ < s_.size() &&
           ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' ||
            s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '-' ||
            s_[pos_] == '+')) {
      ++pos_;
      any = true;
    }
    if (!any) return false;
    out->type_ = Json::Type::kNumber;
    out->number_ = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
    return std::isfinite(out->number_);
  }

  const std::string& s_;
  size_t pos_ = 0;
};

Json Json::Parse(const std::string& text, bool* ok) {
  Json result;
  JsonParser parser(text);
  const bool parsed = parser.ParseValue(&result);
  if (ok) *ok = parsed;
  if (!parsed) return Json();
  return result;
}

// ---------------------------------------------------------------------------
// 取值
// ---------------------------------------------------------------------------

bool Json::AsBool(bool fallback) const {
  if (type_ == Type::kBool) return bool_;
  if (type_ == Type::kNumber) return number_ != 0.0;
  return fallback;
}

double Json::AsNumber(double fallback) const {
  if (type_ == Type::kNumber) return number_;
  if (type_ == Type::kBool) return bool_ ? 1.0 : 0.0;
  return fallback;
}

std::string Json::AsString(const std::string& fallback) const {
  if (type_ == Type::kString) return string_;
  return fallback;
}

bool Json::Has(const std::string& key) const {
  return type_ == Type::kObject && object_.count(key) > 0;
}

const Json& Json::operator[](const std::string& key) const {
  if (type_ != Type::kObject) return NullJson();
  const auto it = object_.find(key);
  return it == object_.end() ? NullJson() : it->second;
}

std::vector<std::string> Json::Keys() const {
  std::vector<std::string> keys;
  if (type_ != Type::kObject) return keys;
  keys.reserve(object_.size());
  for (const auto& entry : object_) keys.push_back(entry.first);
  return keys;
}

size_t Json::Size() const {
  return type_ == Type::kArray ? array_.size() : 0;
}

const Json& Json::At(size_t index) const {
  if (type_ != Type::kArray || index >= array_.size()) return NullJson();
  return array_[index];
}

double Json::Number(const std::string& key, double fallback) const {
  return (*this)[key].AsNumber(fallback);
}

std::string Json::String(const std::string& key,
                         const std::string& fallback) const {
  return (*this)[key].AsString(fallback);
}

bool Json::Bool(const std::string& key, bool fallback) const {
  return (*this)[key].AsBool(fallback);
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

void JsonWriter::Separate() {
  if (need_comma_) buffer_ += ',';
  need_comma_ = true;
}

// 无键的 BeginObject 有两种用法：起一个顶层对象，或在数组里追加一个元素。
// 后者必须先补分隔逗号，否则相邻两个元素会拼成 "{...}{...}"。顶层用法下
// need_comma_ 为 false，Separate() 不会写出任何东西，两种用法都对。
JsonWriter& JsonWriter::BeginObject() {
  Separate();
  buffer_ += '{';
  need_comma_ = false;
  return *this;
}

JsonWriter& JsonWriter::EndObject() {
  buffer_ += '}';
  need_comma_ = true;
  return *this;
}

JsonWriter& JsonWriter::BeginObject(const std::string& key) {
  Separate();
  buffer_ += '"' + JsonEscape(key) + "\":{";
  need_comma_ = false;
  return *this;
}

JsonWriter& JsonWriter::BeginArray(const std::string& key) {
  Separate();
  buffer_ += '"' + JsonEscape(key) + "\":[";
  need_comma_ = false;
  return *this;
}

JsonWriter& JsonWriter::EndArray() {
  buffer_ += ']';
  need_comma_ = true;
  return *this;
}

JsonWriter& JsonWriter::Key(const std::string& key, const std::string& value) {
  Separate();
  buffer_ += '"' + JsonEscape(key) + "\":\"" + JsonEscape(value) + '"';
  return *this;
}

JsonWriter& JsonWriter::Key(const std::string& key, const char* value) {
  return Key(key, std::string(value ? value : ""));
}

JsonWriter& JsonWriter::Key(const std::string& key, double value,
                            int decimals) {
  Separate();
  char buf[48];
  if (!std::isfinite(value)) value = 0.0;
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  buffer_ += '"' + JsonEscape(key) + "\":" + buf;
  return *this;
}

JsonWriter& JsonWriter::Key(const std::string& key, int value) {
  Separate();
  buffer_ += '"' + JsonEscape(key) + "\":" + std::to_string(value);
  return *this;
}

JsonWriter& JsonWriter::Key(const std::string& key, unsigned value) {
  Separate();
  buffer_ += '"' + JsonEscape(key) + "\":" + std::to_string(value);
  return *this;
}

JsonWriter& JsonWriter::Key(const std::string& key, bool value) {
  Separate();
  buffer_ += '"' + JsonEscape(key) + "\":" + (value ? "true" : "false");
  return *this;
}

JsonWriter& JsonWriter::Raw(const std::string& key, const std::string& json) {
  Separate();
  buffer_ += '"' + JsonEscape(key) + "\":" + (json.empty() ? "null" : json);
  return *this;
}

JsonWriter& JsonWriter::Value(const std::string& value) {
  Separate();
  buffer_ += '"' + JsonEscape(value) + '"';
  return *this;
}

std::string JsonWriter::Take() { return std::move(buffer_); }

}  // namespace x30
