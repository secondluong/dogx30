#include "x30/localizer.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

void AddWallX(std::vector<float>* xyz, float x0, float x1, float y, float z) {
  for (float x = x0; x <= x1; x += 0.05f) {
    xyz->push_back(x);
    xyz->push_back(y);
    xyz->push_back(z);
  }
}

void Shift(std::vector<float>* xyz, float dx, float dy) {
  for (size_t i = 0; i < xyz->size(); i += 3) {
    (*xyz)[i] += dx;
    (*xyz)[i + 1] += dy;
  }
}

bool Near(float a, float b, float e) { return std::fabs(a - b) <= e; }

}  // namespace

int main() {
  x30::Localizer loc;
  loc.Seed(0, 0, 0);
  loc.SetImuYaw(0);

  std::vector<float> a;
  AddWallX(&a, 1.0f, 8.0f, 2.0f, 0.6f);
  AddWallX(&a, 1.0f, 8.0f, -2.0f, 0.6f);
  if (!loc.Feed(a.data(), a.size() / 3)) {
    std::fprintf(stderr, "首帧应当直接写入地图\n");
    return 1;
  }

  // 狗往前走 0.4 m，机体系里墙往后退。
  std::vector<float> b = a;
  Shift(&b, -0.40f, 0.0f);
  if (!loc.Feed(b.data(), b.size() / 3)) {
    std::fprintf(stderr, "平移匹配失败 score=%.2f\n", loc.pose().score);
    return 1;
  }
  const x30::LocalizerPose p = loc.pose();
  if (!Near(p.x, 0.40f, 0.08f) || !Near(p.y, 0.0f, 0.08f)) {
    std::fprintf(stderr, "平移不准 x=%.3f y=%.3f（期望 0.40, 0）\n", p.x, p.y);
    return 1;
  }

  // 再走 0.3 m，应对地图而不是只对上一帧，避免累加量化误差。
  std::vector<float> c = a;
  Shift(&c, -0.70f, 0.0f);
  if (!loc.Feed(c.data(), c.size() / 3) || !Near(loc.pose().x, 0.70f, 0.10f)) {
    std::fprintf(stderr, "第二段平移不准 x=%.3f（期望 0.70）\n", loc.pose().x);
    return 1;
  }

  std::printf("localizer_test 通过  x=%.3f score=%.2f cells=%u\n",
              loc.pose().x, loc.pose().score, loc.pose().cells);
  return 0;
}
