#include "x30/gait_coordinator.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

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
                              ResultHandler on_done, bool stepping_hint) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_ || busy_.load()) return false;
  pending_ = true;
  pending_target_ = target;
  pending_style_ = stair_style;
  pending_step_hint_ = stepping_hint;
  pending_handler_ = std::move(on_done);
  cv_.notify_one();
  return true;
}

void GaitCoordinator::ApplyQueuedWhenWalking() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!queued_) {
    if (busy_.load()) apply_queued_requested_ = true;
    return;
  }
  if (pending_ || busy_.load()) {
    apply_queued_requested_ = true;
    return;
  }
  pending_ = true;
  pending_target_ = queued_target_;
  pending_style_ = queued_style_;
  pending_step_hint_ = true;
  pending_handler_ = std::move(queued_handler_);
  queued_ = false;
  apply_queued_requested_ = false;
  cv_.notify_one();
}

void GaitCoordinator::ClearQueued() {
  std::lock_guard<std::mutex> lock(mutex_);
  queued_ = false;
  apply_queued_requested_ = false;
  queued_handler_ = {};
}

void GaitCoordinator::WorkerLoop() {
  while (running_.load()) {
    Gait target;
    HeightMapMode style;
    bool step_hint = false;
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
      step_hint = pending_step_hint_;
      handler = std::move(pending_handler_);
    }

    busy_.store(true);
    Result result = Execute(target, style, step_hint);
    busy_.store(false);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (result.code == "queued") queued_handler_ = handler;
      if (apply_queued_requested_ && queued_ && !pending_) {
        pending_ = true;
        pending_target_ = queued_target_;
        pending_style_ = queued_style_;
        pending_step_hint_ = true;
        pending_handler_ = std::move(queued_handler_);
        queued_ = false;
        apply_queued_requested_ = false;
        cv_.notify_one();
      }
    }
    if (handler) handler(result);
  }
}

GaitCoordinator::Result GaitCoordinator::Execute(Gait target,
                                                 HeightMapMode stair_style,
                                                 bool stepping_hint) {
  if (target == Gait::kLStair) stair_style = HeightMapMode::kSolid;
  const RobotState snapshot = motion_.Snapshot();
  const bool user_step = motion_.UserStepping() || stepping_hint;
  const bool standing = GaitSwitchApply(snapshot.basic_state, snapshot.rl_standing,
                                        snapshot.emergency_source);

  if (JointsLocked(snapshot.basic_state, snapshot.emergency_source) ||
      IsStandSitTransient(snapshot.basic_state)) {
    return {false, "not_stepping",
            std::string("当前不能切步态，") + ToString(snapshot.basic_state)};
  }

  const bool needs_map = RequiresHeightMap(target);
  const bool multiframe = RequiresMultiFrame(target);
  const bool already = snapshot.telemetry_alive && snapshot.gait == target;

  // 力控或停步时允许先选整套配置，但不碰主机；起步后由
  // ApplyQueuedWhenWalking 重新排入，并等待狗主机确认踏步。
  if (!user_step) {
    if (!standing && snapshot.telemetry_alive) {
      return {false, "not_standing",
              std::string("当前不能设置步态，姿态为") +
                  ToString(snapshot.basic_state)};
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queued_ = true;
      queued_target_ = target;
      queued_style_ = stair_style;
    }
    return {true, "queued",
            std::string("已记下") + ToString(target) + "，起步后切换"};
  }

  if (!WaitWalking()) {
    return {false, "not_stepping", "起步未被狗主机确认，步态设置没有执行"};
  }
  // 从匍匐切到越野/楼梯时，主机会先拒绝步态码；正常身高必须先恢复。
  // 反向（目标匍匐）仍是先切步态再降身高，由 MotionClient 的步态屏障保证。
  motion_.ApplyQueuedNormalHeightBeforeGait();

  if (needs_map) {
    // 楼梯要地形图确认，没遥测等于瞎切。
    if (!snapshot.telemetry_alive) {
      return {false, "no_telemetry",
              "与机器狗失联，无法确认切换结果。请检查网线与运动主机登记。"};
    }
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
    // 官方 V1.0.6 要求进入非手动后显式指定速度源；遥控场景仍选手柄。
    terrain_.SetVelSource(VelSource::kHandle);

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
    // 只在离开楼梯/非手动时拉回手动。爬坡、常规不要附带模式、力控、步态码。
    // 上次一点爬坡就发力控，摇杆从走路变成调姿，常规也回不去。
    if (snapshot.control_mode == ControlMode::kNonManual ||
        RequiresHeightMap(snapshot.gait)) {
      motion_.SetControlMode(ControlMode::kManual);
    }
    // 非楼梯：踏步里立刻发。遥测常年报 0 / 收不到，不能拿它当「已经是这档」
    // 或「还没踏步」——静音、低姿就是这样点了像没反应。
    motion_.SetGait(target);
    return {true, "", std::string("已切换到") + ToString(target)};
  }

  if (already) {
    return {true, "", std::string("已处于") + ToString(target)};
  }

  motion_.SetGait(target);

  if (!WaitGaitConfirmed(target)) {
    // 楼梯没切成立刻拉回手动，别把摇杆留在非手动里吃速度。
    motion_.SetControlMode(ControlMode::kManual);
    return {false, "gait_not_applied",
            std::string("切换到") + ToString(target) +
                "未生效。楼梯步态需要感知主机的地形图模块配合，"
                "请确认 192.168.1.105 可达、地形图模块已启动，"
                "且所选踏面类型与实际楼梯相符。"};
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

bool GaitCoordinator::WaitWalking() {
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(cfg_.standstill_timeout_ms);
  while (Clock::now() < deadline) {
    const RobotState s = motion_.Snapshot();
    if (s.telemetry_alive && s.basic_state == BasicState::kStepping) return true;
    SleepMs(50);
  }
  return false;
}

}  // namespace x30
