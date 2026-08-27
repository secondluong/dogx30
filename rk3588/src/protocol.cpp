#include "x30/protocol.hpp"

namespace x30 {

const char* ToString(BasicState s) {
  switch (s) {
    case BasicState::kSitting:
      return "趴下";
    case BasicState::kSitToStand:
      return "起立中";
    case BasicState::kInitialStanding:
      return "初始站立";
    case BasicState::kTorqueStanding:
      return "力控站立";
    case BasicState::kStepping:
      return "踏步";
    case BasicState::kStandToSit:
      return "趴下中";
    case BasicState::kEmergencyOrFall:
      return "急停/跌倒";
    case BasicState::kRl:
      return "RL";
  }
  return "未知";
}

const char* ToString(Gait g) {
  switch (g) {
    case Gait::kWalk:
      return "Walk";
    case Gait::kOffRoad:
      return "越野";
    case Gait::kSlope:
      return "缓坡";
    case Gait::kRun:
      return "Run";
    case Gait::kStair:
      return "楼梯";
    case Gait::kStairMulti:
      return "楼梯(多帧)";
    case Gait::kStair45:
      return "45°楼梯(多帧)";
    case Gait::kLWalk:
      return "L-Walk";
    case Gait::kMountain:
      return "山地";
    case Gait::kSilent:
      return "静音";
    case Gait::kLStair:
      return "L楼梯";
  }
  return "未知";
}

uint32_t GaitCommandCode(Gait g) {
  switch (g) {
    case Gait::kWalk:
      return cmd::kGaitWalk;
    case Gait::kSlope:
      return cmd::kGaitSlope;
    case Gait::kOffRoad:
      return cmd::kGaitOffRoad;
    case Gait::kStair:
      return cmd::kGaitStair;
    case Gait::kStairMulti:
      return cmd::kGaitStairMulti;
    case Gait::kStair45:
      return cmd::kGaitStair45;
    case Gait::kLWalk:
      return cmd::kGaitLWalk;
    case Gait::kMountain:
      return cmd::kGaitMountain;
    case Gait::kSilent:
      return cmd::kGaitSilent;
    case Gait::kLStair:
      return cmd::kGaitLStair;
    // Run 没有独立的切换指令码，遥控器上是 D 键，协议文档未列出。
    case Gait::kRun:
      return 0;
  }
  return 0;
}

GaitLimits LimitsOf(Gait g) {
  switch (g) {
    case Gait::kWalk:
      return {1.5f, 0.15f, 0.45f};
    case Gait::kSlope:
      return {0.7f, 0.25f, 0.5f};
    case Gait::kOffRoad:
      return {0.3f, 0.1f, 0.5f};
    case Gait::kStair:
      return {0.3f, 0.2f, 0.8f};
    case Gait::kStairMulti:
      return {0.6f, 0.2f, 0.8f};
    case Gait::kStair45:
      return {0.3f, 0.2f, 0.8f};
    case Gait::kLWalk:
      return {1.5f, 0.5f, 1.2f};
    case Gait::kMountain:
      return {1.5f, 0.5f, 1.2f};
    case Gait::kSilent:
      return {1.0f, 0.5f, 1.0f};
    case Gait::kRun:
      return {2.5f, 0.8f, 1.2f};
    case Gait::kLStair:
      return {0.3f, 0.2f, 0.8f};
  }
  return {1.5f, 0.15f, 0.45f};
}

const char* ToString(HeightMapMode m) {
  switch (m) {
    case HeightMapMode::kSolid:
      return "实心踏面";
    case HeightMapMode::kGrating:
      return "格栅踏面";
    case HeightMapMode::kNoRiser:
      return "无踢面";
    case HeightMapMode::kMultiFramePrep:
      return "多帧准备";
    case HeightMapMode::kMultiFrame:
      return "多帧";
  }
  return "未知";
}

StepZMax RecommendedStepZMax(Gait g) {
  // 文档附录推荐：Walk / 缓坡 用 8cm，越野与三种楼梯用 28cm。
  // 其余步态没有明确推荐值，保守留在 8cm。
  switch (g) {
    case Gait::kOffRoad:
    case Gait::kStair:
    case Gait::kStairMulti:
    case Gait::kStair45:
    case Gait::kLStair:
      return StepZMax::k28cm;
    default:
      return StepZMax::k8cm;
  }
}

bool RequiresHeightMap(Gait g) {
  return g == Gait::kStair || g == Gait::kStairMulti ||
         g == Gait::kStair45 || g == Gait::kLStair;
}

bool RequiresMultiFrame(Gait g) {
  return g == Gait::kStairMulti || g == Gait::kStair45;
}

}  // namespace x30
