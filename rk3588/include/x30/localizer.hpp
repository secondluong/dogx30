// 扫描到地图的 2D 定位。原厂 /lio_odom 不出数、腿式里程计走路又冻住时，
// 用机体系点云在本机建一张占用栅格，做相关匹配。这不是完整 LIO，
// 没有回环，但比浏览器 20 cm 隔帧相关稳一个数量级：10 Hz 原分辨率、
// 5 cm 格子、对地图而不是对上一帧、IMU 航向当先验。

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace x30 {

struct LocalizerPose {
  float x = 0.0f;
  float y = 0.0f;
  float yaw = 0.0f;
  bool valid = false;
  float score = 0.0f;  // 命中比例 0~1
  uint32_t cells = 0;
};

class Localizer {
 public:
  void Reset();
  void Seed(float x, float y, float yaw);
  bool seeded() const;
  void SetImuYaw(float yaw_rad);

  // xyz 是机体系，n 为点数。匹配成功返回 true。
  bool Feed(const float* xyz, size_t n);

  LocalizerPose pose() const;

 private:
  struct ScanPt {
    float x, y;
  };

  static int64_t Key(int ix, int iy);
  void Extract(const float* xyz, size_t n, std::vector<ScanPt>* out) const;
  int Score(const std::vector<ScanPt>& pts, float x, float y, float yaw) const;
  bool Search(const std::vector<ScanPt>& pts, float* x, float* y, float* yaw,
              float* score_out) const;
  void Insert(const std::vector<ScanPt>& pts, float x, float y, float yaw);

  mutable std::mutex mutex_;
  std::unordered_map<int64_t, uint8_t> map_;
  LocalizerPose pose_{};
  bool seeded_ = false;
  bool have_imu_ = false;
  float imu_yaw_ = 0.0f;
  std::vector<ScanPt> scratch_;
};

}  // namespace x30
