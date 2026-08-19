// 极简 JSON。只覆盖本项目协议用到的子集：扁平对象、字符串、数字、布尔、
// 数组、null。刻意不引第三方库 —— 嵌入式部署时少一个依赖就少一处交叉编译的坑。

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace x30 {

class Json {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Json() = default;
  static Json Parse(const std::string& text, bool* ok = nullptr);

  Type type() const { return type_; }
  bool IsNull() const { return type_ == Type::kNull; }

  // 取值，类型不匹配或键不存在时返回默认值。协议解析里到处是可选字段，
  // 与其每处都判空，不如让取值本身带默认。
  bool AsBool(bool fallback = false) const;
  double AsNumber(double fallback = 0.0) const;
  std::string AsString(const std::string& fallback = "") const;

  bool Has(const std::string& key) const;
  const Json& operator[](const std::string& key) const;

  // 对象的键，按字典序。用于"只认已知键、其余一律报错"的场合 ——
  // 静默忽略拼错的键，表现是「改了、保存了、没生效、也没报错」。
  std::vector<std::string> Keys() const;

  // 数组访问。越界返回空值而不抛异常，与上面取值带默认的风格一致。
  size_t Size() const;
  const Json& At(size_t index) const;

  double Number(const std::string& key, double fallback = 0.0) const;
  std::string String(const std::string& key,
                     const std::string& fallback = "") const;
  bool Bool(const std::string& key, bool fallback = false) const;

 private:
  Type type_ = Type::kNull;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Json> array_;
  std::map<std::string, Json> object_;

  friend class JsonParser;
};

// 顺序拼接 JSON 对象。比构造中间对象再序列化更省事，也更适合 10 Hz 的推送路径。
class JsonWriter {
 public:
  JsonWriter& BeginObject();
  JsonWriter& EndObject();
  JsonWriter& BeginArray(const std::string& key);
  JsonWriter& EndArray();
  JsonWriter& BeginObject(const std::string& key);

  JsonWriter& Key(const std::string& key, const std::string& value);
  JsonWriter& Key(const std::string& key, const char* value);
  JsonWriter& Key(const std::string& key, double value, int decimals = 3);
  JsonWriter& Key(const std::string& key, int value);
  JsonWriter& Key(const std::string& key, unsigned value);
  JsonWriter& Key(const std::string& key, bool value);
  JsonWriter& Value(const std::string& value);  // 数组元素

  // 把一段已经序列化好的 JSON 原样作为某个键的值嵌进来。用于复用别处拼好的
  // 对象，免得同一组字段在两个地方各写一遍 —— 那种副本改一处忘一处时，
  // 表现是协议里少了个字段而没人报错。调用方负责保证 json 本身合法。
  JsonWriter& Raw(const std::string& key, const std::string& json);

  std::string Take();

 private:
  void Separate();

  std::string buffer_;
  bool need_comma_ = false;
};

std::string JsonEscape(const std::string& s);

}  // namespace x30
