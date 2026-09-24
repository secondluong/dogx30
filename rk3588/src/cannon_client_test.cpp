#include "x30/cannon_client.hpp"

#include <cstdio>
#include <cstring>

namespace {

int Fail(const char* msg) {
  std::fprintf(stderr, "FAIL  %s\n", msg);
  return 1;
}

}  // namespace

int main() {
  uint8_t f[x30::kCannonFrameSize];
  x30::CannonCmd cmd;

  x30::EncodeCannonFrame(cmd, f);
  if (std::memcmp(f, x30::kCannonHdr, 5) != 0) return Fail("帧头不是 08 00 00 00 08");
  if (!x30::CannonFrameIdle(f)) return Fail("空指令应当是停止帧");

  cmd.tilt = 0.5f;
  x30::EncodeCannonFrame(cmd, f);
  if (f[5] != 0x02) return Fail("上应置 02");

  cmd = x30::CannonCmd{};
  cmd.tilt = -0.5f;
  x30::EncodeCannonFrame(cmd, f);
  if (f[5] != 0x04) return Fail("下应置 04");

  cmd = x30::CannonCmd{};
  cmd.pan = -0.5f;
  x30::EncodeCannonFrame(cmd, f);
  if (f[5] != 0x08) return Fail("左应置 08");

  cmd = x30::CannonCmd{};
  cmd.pan = 0.5f;
  x30::EncodeCannonFrame(cmd, f);
  if (f[5] != 0x10) return Fail("右应置 10");

  cmd = x30::CannonCmd{};
  std::strncpy(cmd.spray, "fog", 7);
  x30::EncodeCannonFrame(cmd, f);
  if (f[5] != 0x20) return Fail("雾应置 20");

  cmd = x30::CannonCmd{};
  std::strncpy(cmd.spray, "jet", 7);
  x30::EncodeCannonFrame(cmd, f);
  if (f[5] != 0x40) return Fail("柱应置 40");

  cmd = x30::CannonCmd{};
  cmd.fire = true;
  x30::EncodeCannonFrame(cmd, f);
  if (f[5] != 0 || f[7] != 0x02) return Fail("开阀应在第三数据字节置 02");

  cmd.pan = 0.6f;
  cmd.tilt = 0.6f;
  std::strncpy(cmd.spray, "fog", 7);
  cmd.fire = true;
  x30::EncodeCannonFrame(cmd, f);
  if (f[5] != static_cast<uint8_t>(0x02 | 0x10 | 0x20) || f[7] != 0x02) {
    return Fail("上+右+雾+开阀应能同时置位");
  }

  cmd = x30::CannonCmd{};
  cmd.pan = 0.02f;
  cmd.tilt = -0.03f;
  x30::EncodeCannonFrame(cmd, f);
  if (!x30::CannonFrameIdle(f)) return Fail("死区里不该出方向位");

  uint8_t pose[13] = {0x08, 0x00, 0x00, 0x01, 0x00,
                      12, 0x01, 8, 0x02, 0x04, 0, 0, 0};
  x30::CannonTelem t{};
  if (!x30::ParseCannonReply(pose, sizeof(pose), &t) || !t.have_pose) {
    return Fail("角度帧没认出来");
  }
  if (t.pan_deg != 12 || t.pan_dir != 0x01 || t.tilt_deg != 8 ||
      t.tilt_dir != 0x02 || t.limit != 0x04) {
    return Fail("角度帧字段不对");
  }

  uint8_t press[13] = {0x08, 0x00, 0x00, 0x03, 0x00,
                       0xe8, 0x03, 0, 0, 0, 0, 0, 0};  // 1000 / 2500 = 0.4
  t = x30::CannonTelem{};
  if (!x30::ParseCannonReply(press, sizeof(press), &t) || !t.have_pressure) {
    return Fail("压力帧没认出来");
  }
  if (t.pressure_mpa < 0.39f || t.pressure_mpa > 0.41f) {
    return Fail("压力换算不是 小端/2500 MPa");
  }

  std::printf("OK  cannon_client_test\n");
  return 0;
}
