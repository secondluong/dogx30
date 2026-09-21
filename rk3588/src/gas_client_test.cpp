#include "x30/gas_client.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

bool Near(float a, float b) { return std::fabs(a - b) <= 0.01f; }

int Fail(const char* msg) {
  std::fprintf(stderr, "FAIL  %s\n", msg);
  return 1;
}

}  // namespace

int main() {
  x30::GasSlot slots[x30::kGasSlotCount]{};
  slots[0] = {1, 2, 20.9f, x30::GasSlotStatus::kOk};     // O₂
  slots[1] = {2, 3, 12.5f, x30::GasSlotStatus::kOk};     // CO
  slots[2] = {3, 0, 0.0f, x30::GasSlotStatus::kEmpty};
  slots[3] = {4, 6, 0.0f, x30::GasSlotStatus::kOffline};  // H₂S 掉线
  slots[4] = {5, 1, 2.0f, x30::GasSlotStatus::kOk};       // CH₄
  slots[5] = {6, 9, 0.35f, x30::GasSlotStatus::kOk};      // VOC
  slots[6] = {7, 4, 8.0f, x30::GasSlotStatus::kOk};       // NH₃
  slots[7] = {8, 10, 1.2f, x30::GasSlotStatus::kOk};      // SO₂
  slots[8] = {9, 8, 50.0f, x30::GasSlotStatus::kOk};      // H₂
  slots[9] = {10, 7, 4.5f, x30::GasSlotStatus::kOk};      // LEL

  uint8_t frame[x30::kGasFrameSize];
  x30::BuildGasFrame(slots, frame);
  if (frame[0] != 0x3A || frame[1] != 0x02 || frame[2] != 0x00) {
    return Fail("帧头不是 3A 02 00");
  }
  const uint16_t crc = x30::GasCrc16(frame, 63);
  if (frame[63] != static_cast<uint8_t>(crc >> 8) ||
      frame[64] != static_cast<uint8_t>(crc)) {
    return Fail("组帧 CRC 没写成 CRC_H CRC_L");
  }

  x30::GasSlot out[x30::kGasSlotCount]{};
  bool crc_ok = false;
  if (!x30::ParseGasFrame(frame, sizeof(frame), out, &crc_ok)) {
    return Fail("标准 65 字节帧解析失败");
  }
  if (!crc_ok) return Fail("自组帧 CRC 应当通过");
  if (out[0].type_id != 2 || out[0].status != x30::GasSlotStatus::kOk ||
      !Near(out[0].value, 20.9f)) {
    return Fail("O₂ 槽位解析不对");
  }
  if (out[1].type_id != 3 || !Near(out[1].value, 12.5f)) {
    return Fail("CO 槽位解析不对");
  }
  if (out[2].status != x30::GasSlotStatus::kEmpty) {
    return Fail("空槽没认出来");
  }
  if (out[3].status != x30::GasSlotStatus::kOffline || out[3].type_id != 6) {
    return Fail("掉线槽没认出来");
  }
  if (std::strcmp(x30::GasTypeKey(2), "o2") != 0 ||
      std::strcmp(x30::GasTypeUnit(2), "%VOL") != 0) {
    return Fail("气体类型表不对");
  }
  if (x30::GasBoardOfPort(2) != 1 || x30::GasBoardOfPort(3) != 1 ||
      x30::GasBoardOfPort(6) != 2 || x30::GasBoardOfPort(10) != 2) {
    return Fail("端口到板号映射不对");
  }

  // 电量帧拼在后面，应仍能切出气体帧。
  uint8_t mixed[80];
  std::memcpy(mixed, frame, sizeof(frame));
  mixed[65] = 0x0B;
  mixed[66] = 0x00;
  x30::GasSlot mixed_out[x30::kGasSlotCount]{};
  if (!x30::ParseGasFrame(mixed, sizeof(mixed), mixed_out, nullptr) ||
      !Near(mixed_out[0].value, 20.9f)) {
    return Fail("拼包后找不到气体帧");
  }

  // 前缀垃圾 + 气体帧。
  uint8_t prefixed[70];
  prefixed[0] = 0x11;
  prefixed[1] = 0x22;
  std::memcpy(prefixed + 2, frame, sizeof(frame));
  x30::GasSlot pref_out[x30::kGasSlotCount]{};
  if (!x30::ParseGasFrame(prefixed, sizeof(prefixed), pref_out, nullptr) ||
      pref_out[5].type_id != 9) {
    return Fail("带前缀的气体帧解析失败");
  }

  // CRC 坏了也要能用：现场算法若对不上，不能整页空白。
  frame[64] ^= 0xFF;
  x30::GasSlot bad_crc[x30::kGasSlotCount]{};
  bool crc2 = true;
  if (!x30::ParseGasFrame(frame, sizeof(frame), bad_crc, &crc2) || crc2) {
    return Fail("CRC 不对时仍应解析槽位，并标出 crc_ok=false");
  }
  if (!Near(bad_crc[0].value, 20.9f)) {
    return Fail("CRC 失败不该丢掉浓度");
  }

  if (x30::ParseGasFrame(frame, 10, bad_crc, nullptr)) {
    return Fail("过短的包不该被当成一帧");
  }

  const uint8_t pump_ok[] = {0xAA, 0x55, 0x00, 0x0B, 0x04, 0x00, 0x01,
                             0x9F, 0x2A, 0x0D, 0x0A};
  x30::SwitchAck ack;
  if (!x30::ParseSwitchAck(pump_ok, sizeof(pump_ok), &ack) || !ack.ok ||
      !ack.on || ack.cmd != 0x04) {
    return Fail("气泵开应答解析不对");
  }
  const uint8_t light_fail[] = {0xAA, 0x55, 0x00, 0x0B, 0x02, 0x01, 0x01,
                                0x7E, 0xBB, 0x0D, 0x0A};
  if (!x30::ParseSwitchAck(light_fail, sizeof(light_fail), &ack) || ack.ok ||
      ack.cmd != 0x02) {
    return Fail("条纹灯失败应答解析不对");
  }

  uint8_t uwb[x30::kUwbFrameSize];
  x30::BuildUwbFrame(0x01, 15, true, 1234, -450, 120, 9000, uwb);
  if (uwb[0] != 0x3A || uwb[1] != 0x56 || uwb[3] != 15 || uwb[4] != 0x01) {
    return Fail("UWB 帧头不对");
  }
  x30::UwbTag tag;
  bool uwb_crc = false;
  if (!x30::ParseUwbFrame(uwb, sizeof(uwb), &tag, &uwb_crc) || !uwb_crc) {
    return Fail("UWB 标准帧解析失败");
  }
  if (tag.id != 15 || !tag.valid || !Near(tag.x, 1.234f) ||
      !Near(tag.y, -0.45f) || !Near(tag.z, 0.12f) || tag.tick_ms != 9000) {
    return Fail("UWB 坐标解析不对");
  }

  x30::BuildUwbFrame(0x01, 11, false, 999, 999, 999, 1, uwb);
  if (!x30::ParseUwbFrame(uwb, sizeof(uwb), &tag, nullptr) || tag.valid ||
      tag.id != 11 || tag.x != 0.0f) {
    return Fail("无效 UWB 帧应丢弃 XYZ");
  }

  x30::BuildUwbFrame(0x01, 7, true, 100, 0, 0, 1, uwb);
  if (x30::ParseUwbFrame(uwb, sizeof(uwb), &tag, nullptr)) {
    return Fail("非白名单标签不该收下");
  }
  if (x30::UwbTagIndex(15) != 0 || x30::UwbTagIndex(11) != 1 ||
      x30::UwbTagIndex(3) >= 0) {
    return Fail("UWB 白名单下标不对");
  }

  std::printf("gas_client_test 通过\n");
  return 0;
}
