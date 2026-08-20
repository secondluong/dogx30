#include "x30/localizer.hpp"

#include <cmath>

namespace x30 {
namespace {

constexpr float kCell = 0.05f;
constexpr float kInvCell = 20.0f;
constexpr float kZMin = 0.12f;
constexpr float kZMax = 1.70f;
constexpr float kRMin2 = 0.8f * 0.8f;
constexpr float kRMax2 = 16.0f * 16.0f;
constexpr size_t kMaxScan = 900;
constexpr size_t kMaxCells = 220000;

int CellOf(float v) { return static_cast<int>(std::floor(v * kInvCell)); }

}  // namespace

int64_t Localizer::Key(int ix, int iy) {
  return (static_cast<int64_t>(static_cast<uint32_t>(ix)) << 32) |
         static_cast<uint32_t>(iy);
}

void Localizer::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  map_.clear();
  pose_ = LocalizerPose{};
  seeded_ = false;
  have_imu_ = false;
  imu_yaw_ = 0.0f;
}

void Localizer::Seed(float x, float y, float yaw) {
  std::lock_guard<std::mutex> lock(mutex_);
  pose_.x = x;
  pose_.y = y;
  pose_.yaw = yaw;
  pose_.valid = false;
  pose_.score = 0.0f;
  seeded_ = true;
  map_.clear();
}

bool Localizer::seeded() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return seeded_;
}

void Localizer::SetImuYaw(float yaw_rad) {
  std::lock_guard<std::mutex> lock(mutex_);
  imu_yaw_ = yaw_rad;
  have_imu_ = true;
}

LocalizerPose Localizer::pose() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pose_;
}

void Localizer::Extract(const float* xyz, size_t n,
                        std::vector<ScanPt>* out) const {
  out->clear();
  if (n == 0) return;
  const size_t step = n > 4000 ? n / 2000 : 1;
  out->reserve(kMaxScan);
  for (size_t i = 0; i < n && out->size() < kMaxScan; i += step) {
    const float x = xyz[i * 3], y = xyz[i * 3 + 1], z = xyz[i * 3 + 2];
    if (z < kZMin || z > kZMax) continue;
    const float r2 = x * x + y * y;
    if (r2 < kRMin2 || r2 > kRMax2) continue;
    out->push_back({x, y});
  }
}

int Localizer::Score(const std::vector<ScanPt>& pts, float x, float y,
                     float yaw) const {
  const float c = std::cos(yaw), s = std::sin(yaw);
  int hit = 0;
  for (const ScanPt& p : pts) {
    const float wx = x + c * p.x - s * p.y;
    const float wy = y + s * p.x + c * p.y;
    const int ix = CellOf(wx), iy = CellOf(wy);
    if (map_.count(Key(ix, iy)) || map_.count(Key(ix + 1, iy)) ||
        map_.count(Key(ix - 1, iy)) || map_.count(Key(ix, iy + 1)) ||
        map_.count(Key(ix, iy - 1))) {
      ++hit;
    }
  }
  return hit;
}

bool Localizer::Search(const std::vector<ScanPt>& pts, float* x, float* y,
                       float* yaw, float* score_out) const {
  const float yaw0 = have_imu_ ? imu_yaw_ : *yaw;
  float best_x = *x, best_y = *y, best_yaw = yaw0;
  int best = Score(pts, best_x, best_y, best_yaw);

  auto consider = [&](float cx, float cy, float cyaw) {
    const int s = Score(pts, cx, cy, cyaw);
    if (s > best) {
      best = s;
      best_x = cx;
      best_y = cy;
      best_yaw = cyaw;
    }
  };

  // 粗搜：10 cm、3°。狗 1.5 m/s × 0.1 s = 15 cm，窗口 ±40 cm 够用。
  for (int iyaw = -3; iyaw <= 3; ++iyaw) {
    const float yy = yaw0 + iyaw * 0.052f;
    for (int ix = -4; ix <= 4; ++ix) {
      for (int iy = -4; iy <= 4; ++iy) {
        consider(*x + ix * 0.10f, *y + iy * 0.10f, yy);
      }
    }
  }
  // 精搜：2 cm、0.5°。
  const float fx = best_x, fy = best_y, fyaw = best_yaw;
  for (int iyaw = -3; iyaw <= 3; ++iyaw) {
    const float yy = fyaw + iyaw * 0.009f;
    for (int ix = -3; ix <= 3; ++ix) {
      for (int iy = -3; iy <= 3; ++iy) {
        consider(fx + ix * 0.02f, fy + iy * 0.02f, yy);
      }
    }
  }

  const float ratio = pts.empty() ? 0.0f : static_cast<float>(best) / pts.size();
  *score_out = ratio;
  if (ratio < 0.18f) return false;
  *x = best_x;
  *y = best_y;
  *yaw = best_yaw;
  return true;
}

void Localizer::Insert(const std::vector<ScanPt>& pts, float x, float y,
                       float yaw) {
  if (map_.size() >= kMaxCells) return;
  const float c = std::cos(yaw), s = std::sin(yaw);
  for (const ScanPt& p : pts) {
    const float wx = x + c * p.x - s * p.y;
    const float wy = y + s * p.x + c * p.y;
    map_[Key(CellOf(wx), CellOf(wy))] = 1;
    if (map_.size() >= kMaxCells) return;
  }
}

bool Localizer::Feed(const float* xyz, size_t n) {
  Extract(xyz, n, &scratch_);
  if (scratch_.size() < 40) return false;

  std::lock_guard<std::mutex> lock(mutex_);
  if (!seeded_) {
    pose_.x = 0.0f;
    pose_.y = 0.0f;
    pose_.yaw = have_imu_ ? imu_yaw_ : 0.0f;
    seeded_ = true;
  }

  if (!pose_.valid) {
    if (have_imu_) pose_.yaw = imu_yaw_;
    Insert(scratch_, pose_.x, pose_.y, pose_.yaw);
    pose_.valid = true;
    pose_.score = 1.0f;
    pose_.cells = static_cast<uint32_t>(map_.size());
    return true;
  }

  float x = pose_.x, y = pose_.y, yaw = pose_.yaw, score = 0.0f;
  if (have_imu_) yaw = imu_yaw_;
  if (!Search(scratch_, &x, &y, &yaw, &score)) {
    pose_.score = score;
    return false;
  }
  pose_.x = x;
  pose_.y = y;
  pose_.yaw = yaw;
  pose_.score = score;
  Insert(scratch_, x, y, yaw);
  pose_.cells = static_cast<uint32_t>(map_.size());
  return true;
}

}  // namespace x30
