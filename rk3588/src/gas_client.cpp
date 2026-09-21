#include "x30/gas_client.hpp"

#include <cmath>
#include <cstring>
#include <cstdio>

#include "x30/json.hpp"

namespace x30 {
namespace {

using Clock = std::chrono::steady_clock;

struct GasTypeInfo {
  const char* key;
  const char* name;
  const char* unit;
};

// ID 0–10，与协议表一一对应。VOC 已经是苯当量，上位机不要再乘系数。
const GasTypeInfo kTypes[] = {
    {"empty", "空槽", ""},
    {"ch4", "甲烷 CH₄", "%LEL"},
    {"o2", "氧气 O₂", "%VOL"},
    {"co", "一氧化碳 CO", "ppm"},
    {"nh3", "氨气 NH₃", "ppm"},
    {"no2", "二氧化氮 NO₂", "ppm"},
    {"h2s", "硫化氢 H₂S", "ppm"},
    {"lel", "可燃气 LEL", "%LEL"},
    {"h2", "氢气 H₂", "ppm"},
    {"voc", "VOC", "ppm"},
    {"so2", "二氧化硫 SO₂", "ppm"},
};

const GasTypeInfo kUnknown = {"unknown", "未知", ""};

const GasTypeInfo& TypeOf(uint8_t id) {
  if (id < sizeof(kTypes) / sizeof(kTypes[0])) return kTypes[id];
  return kUnknown;
}

float BeFloat32(const uint8_t* p) {
  uint32_t bits = (static_cast<uint32_t>(p[0]) << 24) |
                  (static_cast<uint32_t>(p[1]) << 16) |
                  (static_cast<uint32_t>(p[2]) << 8) |
                  static_cast<uint32_t>(p[3]);
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

int32_t LeI32(const uint8_t* p) {
  const uint32_t u = static_cast<uint32_t>(p[0]) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
  return static_cast<int32_t>(u);
}

uint32_t LeU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void PutLeI32(uint8_t* p, int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v);
  p[0] = static_cast<uint8_t>(u);
  p[1] = static_cast<uint8_t>(u >> 8);
  p[2] = static_cast<uint8_t>(u >> 16);
  p[3] = static_cast<uint8_t>(u >> 24);
}

void PutLeU32(uint8_t* p, uint32_t u) {
  p[0] = static_cast<uint8_t>(u);
  p[1] = static_cast<uint8_t>(u >> 8);
  p[2] = static_cast<uint8_t>(u >> 16);
  p[3] = static_cast<uint8_t>(u >> 24);
}

void PutBeFloat32(uint8_t* p, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  p[0] = static_cast<uint8_t>(bits >> 24);
  p[1] = static_cast<uint8_t>(bits >> 16);
  p[2] = static_cast<uint8_t>(bits >> 8);
  p[3] = static_cast<uint8_t>(bits);
}

bool IsOfflineBits(const uint8_t* p) {
  return p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF;
}

bool IsZeroBits(const uint8_t* p) {
  return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0;
}

void DecodeSlot(const uint8_t* p, int index, GasSlot* out) {
  const uint8_t port = p[0];
  const uint8_t id = p[1];
  out->port = (port >= 1 && port <= kGasSlotCount)
                  ? port
                  : static_cast<uint8_t>(index + 1);
  out->type_id = id;
  if (id == 0 && IsZeroBits(p + 2)) {
    out->status = GasSlotStatus::kEmpty;
    out->value = 0.0f;
    return;
  }
  if (IsOfflineBits(p + 2)) {
    out->status = GasSlotStatus::kOffline;
    out->value = 0.0f;
    return;
  }
  const float v = BeFloat32(p + 2);
  if (!std::isfinite(v)) {
    out->status = GasSlotStatus::kOffline;
    out->value = 0.0f;
    return;
  }
  out->status = GasSlotStatus::kOk;
  out->value = v;
}

}  // namespace

const char* GasTypeKey(uint8_t id) { return TypeOf(id).key; }

int GasBoardOfPort(uint8_t port) {
  if (port >= 1 && port <= 5) return 1;
  if (port >= 6 && port <= 10) return 2;
  return 0;
}
const char* GasTypeName(uint8_t id) { return TypeOf(id).name; }
const char* GasTypeUnit(uint8_t id) { return TypeOf(id).unit; }

const char* GasStatusText(GasSlotStatus s) {
  switch (s) {
    case GasSlotStatus::kEmpty:
      return "empty";
    case GasSlotStatus::kOk:
      return "ok";
    case GasSlotStatus::kOffline:
      return "offline";
  }
  return "empty";
}

uint16_t GasCrc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 1u) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001u)
                       : static_cast<uint16_t>(crc >> 1);
    }
  }
  return crc;
}

bool ParseGasFrame(const uint8_t* data, size_t len, GasSlot slots[kGasSlotCount],
                   bool* crc_ok) {
  if (crc_ok) *crc_ok = false;
  if (data == nullptr || slots == nullptr || len < kGasFrameSize) return false;

  size_t off = static_cast<size_t>(-1);
  for (size_t i = 0; i + kGasFrameSize <= len; ++i) {
    if (data[i] == 0x3A && data[i + 1] == 0x02 && data[i + 2] == 0x00) {
      off = i;
      break;
    }
  }
  if (off == static_cast<size_t>(-1)) return false;

  const uint8_t* frame = data + off;
  const uint16_t got =
      (static_cast<uint16_t>(frame[63]) << 8) | static_cast<uint16_t>(frame[64]);
  const uint16_t want = GasCrc16(frame, 63);
  if (crc_ok) *crc_ok = (got == want);

  GasSlot decoded[kGasSlotCount];
  for (int i = 0; i < kGasSlotCount; ++i) {
    DecodeSlot(frame + 3 + i * 6, i, &decoded[i]);
  }

  // 按端口号落到 0..9。端口字节坏了 DecodeSlot 已经用槽位序号兜过。
  for (int i = 0; i < kGasSlotCount; ++i) {
    const int idx = decoded[i].port - 1;
    if (idx < 0 || idx >= kGasSlotCount) continue;
    slots[idx] = decoded[i];
  }
  return true;
}

void BuildGasFrame(const GasSlot slots[kGasSlotCount],
                   uint8_t out[kGasFrameSize]) {
  out[0] = 0x3A;
  out[1] = 0x02;
  out[2] = 0x00;
  for (int i = 0; i < kGasSlotCount; ++i) {
    uint8_t* p = out + 3 + i * 6;
    const GasSlot& s = slots[i];
    p[0] = s.port ? s.port : static_cast<uint8_t>(i + 1);
    p[1] = s.type_id;
    if (s.status == GasSlotStatus::kOffline) {
      p[2] = p[3] = p[4] = p[5] = 0xFF;
    } else if (s.status == GasSlotStatus::kEmpty || s.type_id == 0) {
      p[2] = p[3] = p[4] = p[5] = 0x00;
      p[1] = 0;
    } else {
      PutBeFloat32(p + 2, s.value);
    }
  }
  const uint16_t crc = GasCrc16(out, 63);
  out[63] = static_cast<uint8_t>(crc >> 8);
  out[64] = static_cast<uint8_t>(crc);
}

int UwbTagIndex(uint8_t id) {
  for (int i = 0; i < kUwbTagCount; ++i) {
    if (kUwbTagIds[i] == id) return i;
  }
  return -1;
}

bool ParseUwbFrame(const uint8_t* data, size_t len, UwbTag* out, bool* crc_ok) {
  if (crc_ok) *crc_ok = false;
  if (data == nullptr || out == nullptr || len < kUwbFrameSize) return false;

  size_t off = static_cast<size_t>(-1);
  for (size_t i = 0; i + kUwbFrameSize <= len; ++i) {
    if (data[i] != 0x3A || data[i + 1] != 0x56) continue;
    if (UwbTagIndex(data[i + 3]) < 0) continue;
    off = i;
    break;
  }
  if (off == static_cast<size_t>(-1)) return false;

  const uint8_t* f = data + off;
  const uint16_t got =
      (static_cast<uint16_t>(f[21]) << 8) | static_cast<uint16_t>(f[22]);
  const uint16_t want = GasCrc16(f, 21);
  if (crc_ok) *crc_ok = (got == want);

  out->id = f[3];
  out->subtype = f[2];
  out->heard = true;
  out->valid = f[4] != 0;
  out->tick_ms = LeU32(f + 17);
  if (out->valid) {
    out->x = static_cast<float>(LeI32(f + 5)) / 1000.0f;
    out->y = static_cast<float>(LeI32(f + 9)) / 1000.0f;
    out->z = static_cast<float>(LeI32(f + 13)) / 1000.0f;
  } else {
    out->x = out->y = out->z = 0.0f;
  }
  return true;
}

void BuildUwbFrame(uint8_t subtype, uint8_t id, bool valid, int32_t x_mm,
                   int32_t y_mm, int32_t z_mm, uint32_t tick_ms,
                   uint8_t out[kUwbFrameSize]) {
  out[0] = 0x3A;
  out[1] = 0x56;
  out[2] = subtype;
  out[3] = id;
  out[4] = valid ? 0x01 : 0x00;
  PutLeI32(out + 5, x_mm);
  PutLeI32(out + 9, y_mm);
  PutLeI32(out + 13, z_mm);
  PutLeU32(out + 17, tick_ms);
  const uint16_t crc = GasCrc16(out, 21);
  out[21] = static_cast<uint8_t>(crc >> 8);
  out[22] = static_cast<uint8_t>(crc);
}

GasClient::GasClient(GasClientConfig config) : cfg_(config) {
  for (int i = 0; i < kUwbTagCount; ++i) uwb_.tags[i].id = kUwbTagIds[i];
}

GasClient::~GasClient() { Stop(); }

bool GasClient::Start(std::string* error) {
  if (running_.load()) return true;
  if (!rx_.Open(cfg_.local_port, error)) return false;
  running_.store(true);
  thread_ = std::thread(&GasClient::RxLoop, this);
  return true;
}

void GasClient::Stop() {
  if (!running_.exchange(false)) {
    rx_.Close();
    return;
  }
  rx_.Close();
  if (thread_.joinable()) thread_.join();
}

GasSnapshot GasClient::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  GasSnapshot s = snap_;
  if (s.heard) {
    const auto age = Clock::now() - last_frame_;
    s.alive = age < std::chrono::milliseconds(cfg_.timeout_ms);
  }
  return s;
}

std::string GasClient::Json() const {
  const GasSnapshot s = Snapshot();
  JsonWriter w;
  w.BeginObject()
      .Key("alive", s.alive)
      .Key("heard", s.heard)
      .BeginArray("slots");
  for (int i = 0; i < kGasSlotCount; ++i) {
    const GasSlot& sl = s.slots[i];
    const uint8_t port = sl.port ? sl.port : static_cast<uint8_t>(i + 1);
    const uint8_t id = sl.type_id;
    w.BeginObject()
        .Key("port", static_cast<int>(port))
        .Key("board", GasBoardOfPort(port))
        .Key("id", static_cast<int>(id))
        .Key("key", GasTypeKey(id))
        .Key("name", GasTypeName(id))
        .Key("unit", GasTypeUnit(id))
        .Key("status", GasStatusText(sl.status));
    if (sl.status == GasSlotStatus::kOk) {
      w.Key("value", sl.value, 2);
    }
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return w.Take();
}

UwbSnapshot GasClient::SnapshotUwb() const {
  std::lock_guard<std::mutex> lock(mutex_);
  UwbSnapshot s = uwb_;
  if (s.heard) {
    s.alive = false;
    const auto now = Clock::now();
    for (int i = 0; i < kUwbTagCount; ++i) {
      if (!s.tags[i].heard) continue;
      const auto age = now - uwb_last_[i];
      if (age >= std::chrono::milliseconds(cfg_.uwb_timeout_ms)) {
        s.tags[i].valid = false;
      } else if (s.tags[i].valid) {
        s.alive = true;
      }
    }
  }
  return s;
}

std::string GasClient::UwbJson() const {
  const UwbSnapshot s = SnapshotUwb();
  JsonWriter w;
  w.BeginObject().Key("alive", s.alive).Key("heard", s.heard).BeginArray("tags");
  for (int i = 0; i < kUwbTagCount; ++i) {
    const UwbTag& t = s.tags[i];
    w.BeginObject()
        .Key("id", static_cast<int>(t.id ? t.id : kUwbTagIds[i]))
        .Key("valid", t.valid)
        .Key("heard", t.heard);
    if (t.valid) {
      w.Key("x", t.x, 3).Key("y", t.y, 3).Key("z", t.z, 3);
      w.Key("tick", static_cast<unsigned>(t.tick_ms));
    }
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return w.Take();
}

bool GasClient::SendTo(const std::string& ip, uint16_t port, const void* data,
                       size_t len) {
  return rx_.SendTo(ip, port, data, len);
}

void GasClient::RxLoop() {
  uint8_t buffer[512];
  while (running_.load()) {
    std::string src_ip;
    uint16_t src_port = 0;
    const int n =
        rx_.RecvFrom(buffer, sizeof(buffer), 200, &src_ip, &src_port);
    if (n <= 0) continue;
    if (on_peer_ && !src_ip.empty() && src_port != 0) {
      on_peer_(src_ip, src_port);
    }
    GasSlot slots[kGasSlotCount]{};
    bool crc_ok = false;
    if (ParseGasFrame(buffer, static_cast<size_t>(n), slots, &crc_ok)) {
      ApplyFrame(slots);
      if (!logged_first_) {
        logged_first_ = true;
        std::printf("气体首帧已解析（%d 字节%s）\n", n,
                    crc_ok ? "" : "，CRC 未对上，已按槽位内容采用");
      }
    }
    for (size_t i = 0; i + kUwbFrameSize <= static_cast<size_t>(n); ++i) {
      if (buffer[i] != 0x3A || buffer[i + 1] != 0x56) continue;
      UwbTag tag;
      bool uwb_crc = false;
      if (!ParseUwbFrame(buffer + i, kUwbFrameSize, &tag, &uwb_crc)) continue;
      ApplyUwb(tag);
      if (!logged_uwb_) {
        logged_uwb_ = true;
        std::printf("UWB 标签首帧已解析 id=%u %s（%d 字节%s）\n",
                    static_cast<unsigned>(tag.id),
                    tag.valid ? "有效" : "无效", n,
                    uwb_crc ? "" : "，CRC 未对上，已按内容采用");
      }
      i += kUwbFrameSize - 1;
    }
    SwitchAck ack;
    if (on_switch_ack_ &&
        ParseSwitchAck(buffer, static_cast<size_t>(n), &ack)) {
      on_switch_ack_(ack);
    }
  }
}

void GasClient::ApplyFrame(const GasSlot slots[kGasSlotCount]) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < kGasSlotCount; ++i) snap_.slots[i] = slots[i];
  snap_.heard = true;
  snap_.alive = true;
  last_frame_ = Clock::now();
}

void GasClient::ApplyUwb(const UwbTag& tag) {
  const int idx = UwbTagIndex(tag.id);
  if (idx < 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  uwb_.tags[idx] = tag;
  uwb_.tags[idx].id = kUwbTagIds[idx];
  uwb_.heard = true;
  if (tag.valid) uwb_.alive = true;
  uwb_last_[idx] = Clock::now();
}

}  // namespace x30
