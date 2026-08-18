// PointCloud2 解析、体素降采样、量化打包。
//
// 带宽是这里唯一的设计约束。四台 Mid-360 合起来每秒约 80 万点，
// 原始 XYZI 是 16 字节/点，也就是 100 Mbps 量级 —— 而 MESH 实测只有十几 Mbps，
// 还要分给视频。所以必须在板子上砍到 2 Mbps 以内再下行。
//
// 砍法有三层：先按距离裁剪，再体素降采样，最后把 float32 坐标量化成 int16。
// 量化这一步单独就省一半：以 ±40 m 的包围盒算，int16 的分辨率是 1.2 mm，
// 远小于雷达本身的精度，等于白拿。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace x30 {

struct PointCloudConfig {
  float voxel_size = 0.10f;    // 米。降采样栅格边长
  float min_range = 0.4f;      // 米。滤掉打到狗自己身上的点
  float max_range = 30.0f;     // 米。远处点稀疏且对遥控无用
  uint32_t max_points = 20000; // 硬上限，直接决定单帧字节数
  int target_hz = 2;           // 从 10 Hz 抽帧到这个频率

  // 超限时自动放大体素。开着的话点数会自己收敛到 max_points 附近，
  // 不用为不同场地手工调参 —— 楼道和空旷场地的点密度差很多。
  bool adaptive_voxel = true;
};

// 解析出来的一帧，已经是量化前的稀疏点。
struct PointCloudFrame {
  uint32_t seq = 0;
  uint64_t stamp_ms = 0;
  std::string frame_id;
  std::vector<float> xyz;  // 3 个一组
};

// 解析 sensor_msgs/PointCloud2 的原始消息体。
// 只取 x/y/z 三个 float32 字段，其余字段（intensity、tag、line 等）跳过。
bool ParsePointCloud2(const uint8_t* data, size_t len, PointCloudFrame* out,
                      std::string* error);

// 降采样 + 量化 + 打包成下行二进制帧。
//
// 线路格式（小端）：
//   0  magic   "X30C"        4B
//   4  version 1             1B
//   5  flags                 1B
//   6  reserved              2B
//   8  seq                   4B
//  12  stamp_ms              8B
//  20  origin x,y,z  float32 12B
//  32  scale         float32  4B   量化单位对应的米数
//  36  count                 4B
//  40  点数据：count × (int16 x, int16 y, int16 z)
//
// 解码方式：世界坐标 = origin + int16 值 × scale。
class PointCloudEncoder {
 public:
  explicit PointCloudEncoder(PointCloudConfig config) : cfg_(config) {}

  // 按 target_hz 抽帧。返回 false 表示这一帧该丢。
  bool ShouldEmit(uint64_t now_ms);

  // 编码到 out。out 会被清空复用，避免每帧重新分配上百 KB。
  void Encode(const PointCloudFrame& frame, std::vector<uint8_t>* out);

  // 上一帧实际使用的体素边长。自适应开启时会变，供遥测展示。
  float effective_voxel() const { return effective_voxel_; }
  uint32_t last_point_count() const { return last_count_; }

  void set_config(const PointCloudConfig& c) {
    cfg_ = c;
    effective_voxel_ = c.voxel_size;
  }
  const PointCloudConfig& config() const { return cfg_; }

 private:
  PointCloudConfig cfg_;
  float effective_voxel_ = 0.10f;
  uint32_t last_count_ = 0;
  uint64_t last_emit_ms_ = 0;
  std::vector<int64_t> voxel_keys_;  // 复用，避免每帧分配
};

}  // namespace x30
