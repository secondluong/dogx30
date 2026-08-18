#include "x30/gait_coordinator.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace x30 {
namespace {

using Clock = std::chrono::steady_clock;

void SleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

GaitCoordinator::GaitCoordinator(MotionClient& motion, TerrainClient& terrain,
                                 GaitCoordinatorConfig config)
    : motion_(motion), terrain_(terrain), cfg_(config) {}

GaitCoordinator::~GaitCoordinator() { Stop(); }

void GaitCoordinator::Start() {
  if (running_.exchange(true)) return;
  worker_ = std::thread(&GaitCoordinator::WorkerLoop, this);
}

void GaitCoordinator::Stop() {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool GaitCoordinator::Request(Gait target, HeightMapMode stair_style,
                              ResultHandler on_done) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_ || busy_.load()) return false;
  pending_ = true;
  pending_target_ = target;
  pending_style_ = stair_style;
  pending_handler_ = std::move(on_done);
  cv_.notify_one();
  return true;
}

void GaitCoordinator::WorkerLoop() {
  while (running_.load()) {
    Gait target;
    HeightMapMode style;
    ResultHandler handler;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(200),
                   [this] { return pending_ || !running_.load(); });
      if (!running_.load()) return;
      if (!pending_) continue;
      pending_ = false;
      target = pending_target_;
      style = pending_style_;
      handler = std::move(pending_handler_);
    }

    busy_.store(true);
    Result result = Execute(target, style);
    busy_.store(false);

    if (handler) handler(result);
  }
}

GaitCoordinator::Result GaitCoordinator::Execute(Gait target,
                                                 HeightMapMode stair_style) {
  const RobotState snapshot = motion_.Snapshot();

  if (!snapshot.telemetry_alive) {
    return {false, "no_telemetry",
            "与机器狗失联，无法确认切换结果。请检查网线与运动主机登记。"};
  }
  if (snapshot.gait == target) {
    return {true, "", std::string("已处于") + ToString(target)};
  }
  if (snapshot.basic_state != BasicState::kStepping) {
    return {false, "not_stepping",
            std::string("仅踏步态下可切换步态，当前为") +
                ToString(snapshot.basic_state)};
  }

  const bool needs_map = RequiresHeightMap(target);
  const bool multiframe = RequiresMultiFrame(target);

  if (needs_map) {
    // 多帧模式只能在静止时切换，这是文档的硬性约束。
    if (multiframe) {
      motion_.ReleaseAxes();
      if (!WaitStandstill()) {
        return {false, "not_standstill",
                "多帧楼梯模式要求机器狗完全静止，请先松开摇杆等它停稳。"};
      }
    }

    // 地形图的修正结果只在非手动模式下参与速度链路，手动模式下白设。
    motion_.SetControlMode(ControlMode::kNonManual);

    terrain_.SetStepZMax(StepZMax::k28cm);

    if (multiframe) {
      terrain_.SetHeightMapMode(HeightMapMode::kMultiFramePrep);
      SleepMs(cfg_.multiframe_prep_delay_ms);
      terrain_.SetHeightMapMode(HeightMapMode::kMultiFrame);
    } else {
      terrain_.SetHeightMapMode(stair_style);
    }
    SleepMs(cfg_.settle_delay_ms);
  } else {
    terrain_.SetStepZMax(RecommendedStepZMax(target));
  }

  motion_.SetGait(target);

  if (!WaitGaitConfirmed(target)) {
    if (needs_map) {
      return {false, "gait_not_applied",
              std::string("切换到") + ToString(target) +
                  "未生效。楼梯步态需要感知主机的地形图模块配合，"
                  "请确认 192.168.1.105 可达、地形图模块已启动，"
                  "且所选踏面类型与实际楼梯相符。"};
    }
    return {false, "gait_not_applied",
            std::string("切换到") + ToString(target) + "未生效。"};
  }

  return {true, "", std::string("已切换到") + ToString(target)};
}

bool GaitCoordinator::WaitStandstill() {
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(cfg_.standstill_timeout_ms);
  while (Clock::now() < deadline) {
    const RobotState s = motion_.Snapshot();
    if (std::fabs(s.vel_x) < cfg_.standstill_eps &&
        std::fabs(s.vel_y) < cfg_.standstill_eps &&
        std::fabs(s.vel_yaw) < cfg_.standstill_eps) {
      return true;
    }
    SleepMs(50);
  }
  return false;
}

bool GaitCoordinator::WaitGaitConfirmed(Gait target) {
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(cfg_.gait_confirm_timeout_ms);
  while (Clock::now() < deadline) {
    if (motion_.Snapshot().gait == target) return true;
    SleepMs(50);
  }
  return false;
}

}  // namespace x30
