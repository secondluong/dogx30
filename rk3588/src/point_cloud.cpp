#include "x30/point_cloud.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace x30 {
namespace {

// ROS 序列化都是小端，板子和 x86 也都是小端，直接 memcpy 即可。
// 但仍然显式按字节读，免得将来换到大端平台时悄悄出错。
bool ReadU32(const uint8_t* d, size_t len, size_t* pos, uint32_t* out) {
  if (*pos + 4 > len) return false;
  *out = static_cast<uint32_t>(d[*pos]) |
         (static_cast<uint32_t>(d[*pos + 1]) << 8) |
         (static_cast<uint32_t>(d[*pos + 2]) << 16) |
         (static_cast<uint32_t>(d[*pos + 3]) << 24);
  *pos += 4;
  return true;
}

bool ReadU8(const uint8_t* d, size_t len, size_t* pos, uint8_t* out) {
  if (*pos + 1 > len) return false;
  *out = d[*pos];
  *pos += 1;
  return true;
}

bool ReadString(const uint8_t* d, size_t len, size_t* pos, std::string* out) {
  uint32_t n = 0;
  if (!ReadU32(d, len, pos, &n)) return false;
  if (*pos + n > len) return false;
  out->assign(reinterpret_cast<const char*>(d + *pos), n);
  *pos += n;
  return true;
}

float ReadF32At(const uint8_t* d, size_t offset) {
  float f = 0.0f;
  std::memcpy(&f, d + offset, 4);
  return f;
}

void PutU16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void PutU32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void PutU64(std::vector<uint8_t>* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
  }
}

void PutF32(std::vector<uint8_t>* out, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  PutU32(out, bits);
}

constexpr uint8_t kFloat32 = 7;

}  // namespace

bool ParsePointCloud2(const uint8_t* data, size_t len, PointCloudFrame* out,
                      std::string* error) {
  size_t pos = 0;

  uint32_t seq = 0, stamp_sec = 0, stamp_nsec = 0;
  if (!ReadU32(data, len, &pos, &seq) ||
      !ReadU32(data, len, &pos, &stamp_sec) ||
      !ReadU32(data, len, &pos, &stamp_nsec) ||
      !ReadString(data, len, &pos, &out->frame_id)) {
    if (error) *error = "消息头截断";
    return false;
  }
  out->seq = seq;
  out->stamp_ms = static_cast<uint64_t>(stamp_sec) * 1000ull +
                  stamp_nsec / 1000000ull;

  uint32_t height = 0, width = 0;
  if (!ReadU32(data, len, &pos, &height) || !ReadU32(data, len, &pos, &width)) {
    if (error) *error = "缺少 height/width";
    return false;
  }

  uint32_t field_count = 0;
  if (!ReadU32(data, len, &pos, &field_count) || field_count > 64) {
    if (error) *error = "字段数异常";
    return false;
  }

  size_t off_x = 0, off_y = 0, off_z = 0;
  bool has_x = false, has_y = false, has_z = false;
  for (uint32_t i = 0; i < field_count; ++i) {
    std::string name;
    uint32_t offset = 0, count = 0;
    uint8_t datatype = 0;
    if (!ReadString(data, len, &pos, &name) ||
        !ReadU32(data, len, &pos, &offset) ||
        !ReadU8(data, len, &pos, &datatype) ||
        !ReadU32(data, len, &pos, &count)) {
      if (error) *error = "字段表截断";
      return false;
    }
    // 只认 float32 的 xyz。Livox 驱动出来的就是这种；
    // 万一遇到 float64 的，宁可明确报错也不要默默解析出垃圾坐标。
    if (name == "x" && datatype == kFloat32) { off_x = offset; has_x = true; }
    if (name == "y" && datatype == kFloat32) { off_y = offset; has_y = true; }
    if (name == "z" && datatype == kFloat32) { off_z = offset; has_z = true; }
  }
  if (!has_x || !has_y || !has_z) {
    if (error) *error = "点云里没有 float32 的 x/y/z 字段";
    return false;
  }

  uint8_t is_bigendian = 0;
  uint32_t point_step = 0, row_step = 0;
  if (!ReadU8(data, len, &pos, &is_bigendian) ||
      !ReadU32(data, len, &pos, &point_step) ||
      !ReadU32(data, len, &pos, &row_step)) {
    if (error) *error = "缺少 point_step/row_step";
    return false;
  }
  if (is_bigendian != 0) {
    if (error) *error = "不支持大端点云";
    return false;
  }
  if (point_step < 12) {
    if (error) *error = "point_step 过小";
    return false;
  }

  uint32_t data_len = 0;
  if (!ReadU32(data, len, &pos, &data_len) || pos + data_len > len) {
    if (error) *error = "点数据截断";
    return false;
  }
  const uint8_t* points = data + pos;

  const size_t n = data_len / point_step;
  if (off_x + 4 > point_step || off_y + 4 > point_step ||
      off_z + 4 > point_step) {
    if (error) *error = "字段偏移越界";
    return false;
  }

  out->xyz.clear();
  out->xyz.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = points + i * point_step;
    out->xyz.push_back(ReadF32At(p, off_x));
    out->xyz.push_back(ReadF32At(p, off_y));
    out->xyz.push_back(ReadF32At(p, off_z));
  }
  return true;
}

bool PointCloudEncoder::ShouldEmit(uint64_t now_ms) {
  const int hz = cfg_.target_hz > 0 ? cfg_.target_hz : 1;
  const uint64_t interval = 1000ull / static_cast<uint64_t>(hz);
  if (last_emit_ms_ != 0 && now_ms - last_emit_ms_ < interval) return false;
  last_emit_ms_ = now_ms;
  return true;
}

void PointCloudEncoder::Encode(const PointCloudFrame& frame,
                               std::vector<uint8_t>* out) {
  const float voxel = effective_voxel_ > 1e-4f ? effective_voxel_ : 0.1f;
  const float inv_voxel = 1.0f / voxel;
  const float min_sq = cfg_.min_range * cfg_.min_range;
  const float max_sq = cfg_.max_range * cfg_.max_range;

  // 体素去重。键用 int64 打包三个 21 位栅格坐标，够覆盖 ±100 万格；
  // 以 10 cm 的格子算是 ±100 km，远超雷达量程。
  std::unordered_set<int64_t> seen;
  seen.reserve(cfg_.max_points * 2);

  std::vector<float> kept;
  kept.reserve(cfg_.max_points * 3);

  float min_x = 0, min_y = 0, min_z = 0;
  float max_x = 0, max_y = 0, max_z = 0;
  bool first = true;

  const size_t n = frame.xyz.size() / 3;
  for (size_t i = 0; i < n; ++i) {
    const float x = frame.xyz[i * 3 + 0];
    const float y = frame.xyz[i * 3 + 1];
    const float z = frame.xyz[i * 3 + 2];

    // NaN 会污染包围盒，导致整帧坐标压成一个点。Livox 的无效点就是 NaN。
    if (std::isnan(x) || std::isnan(y) || std::isnan(z)) continue;

    // 机体系：距离相对雷达原点。世界系：相对狗当前位姿，否则走出十几米
    // 后原点附近的墙会被当成「狗身上的点」裁掉，周围的点反而留下。
    const float dx = frame.world ? x - frame.robot_x : x;
    const float dy = frame.world ? y - frame.robot_y : y;
    const float d_sq = dx * dx + dy * dy + z * z;
    if (d_sq < min_sq || d_sq > max_sq) continue;

    const auto gx = static_cast<int64_t>(std::floor(x * inv_voxel));
    const auto gy = static_cast<int64_t>(std::floor(y * inv_voxel));
    const auto gz = static_cast<int64_t>(std::floor(z * inv_voxel));
    const int64_t key = ((gx & 0x1FFFFF) << 42) | ((gy & 0x1FFFFF) << 21) |
                        (gz & 0x1FFFFF);
    if (!seen.insert(key).second) continue;

    kept.push_back(x);
    kept.push_back(y);
    kept.push_back(z);

    if (first) {
      min_x = max_x = x;
      min_y = max_y = y;
      min_z = max_z = z;
      first = false;
    } else {
      min_x = std::min(min_x, x); max_x = std::max(max_x, x);
      min_y = std::min(min_y, y); max_y = std::max(max_y, y);
      min_z = std::min(min_z, z); max_z = std::max(max_z, z);
    }
  }

  size_t count = kept.size() / 3;

  // 还是超了就等距抽稀。这一帧先把带宽压住，下一帧靠自适应体素收敛，
  // 不能指望调大体素立刻生效 —— 那要重算整帧。
  size_t stride = 1;
  if (count > cfg_.max_points) {
    stride = (count + cfg_.max_points - 1) / cfg_.max_points;
    count = (kept.size() / 3 + stride - 1) / stride;
  }

  if (cfg_.adaptive_voxel) {
    const size_t raw = kept.size() / 3;
    if (raw > cfg_.max_points) {
      effective_voxel_ = std::min(effective_voxel_ * 1.25f, 1.0f);
    } else if (raw < cfg_.max_points / 2) {
      effective_voxel_ =
          std::max(effective_voxel_ * 0.9f, cfg_.voxel_size);
    }
  }

  // 量化：以包围盒最小角为原点，跨度均分到 uint16 的取值区间。
  // 用 65000 而不是 65535，给浮点舍入留出余量 —— 溢出会绕回成 0，
  // 表现为远处的点突然跳到原点，很难查。
  const float span = std::max({max_x - min_x, max_y - min_y, max_z - min_z,
                               0.001f});
  const float scale = span / 65000.0f;
  const float inv_scale = 1.0f / scale;

  out->clear();
  out->reserve(40 + count * 6);

  out->push_back('X'); out->push_back('3');
  out->push_back('0'); out->push_back('C');
  out->push_back(1);   // version
  out->push_back(frame.world ? kCloudFlagWorld : 0);
  PutU16(out, 0);      // reserved
  PutU32(out, frame.seq);
  PutU64(out, frame.stamp_ms);
  PutF32(out, min_x);
  PutF32(out, min_y);
  PutF32(out, min_z);
  PutF32(out, scale);
  PutU32(out, static_cast<uint32_t>(count));

  for (size_t i = 0; i * stride < kept.size() / 3; ++i) {
    const size_t idx = i * stride;
    const auto qx = static_cast<uint16_t>(
        std::lround((kept[idx * 3 + 0] - min_x) * inv_scale));
    const auto qy = static_cast<uint16_t>(
        std::lround((kept[idx * 3 + 1] - min_y) * inv_scale));
    const auto qz = static_cast<uint16_t>(
        std::lround((kept[idx * 3 + 2] - min_z) * inv_scale));
    PutU16(out, qx);
    PutU16(out, qy);
    PutU16(out, qz);
  }

  last_count_ = static_cast<uint32_t>(count);
}

}  // namespace x30
