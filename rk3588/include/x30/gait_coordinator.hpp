// 步态切换编排。
//
// 为什么需要单独一层：楼梯步态不是发一条指令就成的，要按固定顺序动四个开关，
// 分布在两台主机上 ——
//
//   1. 非手动模式（运动主机）  否则地形图的修正结果不参与速度链路
//   2. 障碍高度阈值 28cm（感知主机）  留在 8cm 会把台阶当障碍挡住
//   3. 地形图模式（感知主机）  多帧还要先"准备"再"启用"，且只能静止时切
//   4. 步态指令（运动主机）
//
// 顺序错了不会报错，只是静默不生效，现场很难查。所以把它收敛成一个动作，
// 不把四个开关暴露给操作员。
//
// 地形图通道收不到确认，但运动主机的步态遥测可以当确认用：地形图没配好时
// 步态切不过去。切完等遥测确认，超时就带着诊断信息报错。
//
// 线程模型：内部一个工作线程串行处理请求，同一时刻只允许一次切换在途。

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "x30/motion_client.hpp"
#include "x30/protocol.hpp"
#include "x30/terrain_client.hpp"

namespace x30 {

struct GaitCoordinatorConfig {
  // 发出步态指令后等待遥测确认的时间。超时即判定未生效。
  int gait_confirm_timeout_ms = 2000;

  // 多帧模式要求静止，这是等待机器狗停下的上限。
  int standstill_timeout_ms = 3000;

  // 判定"静止"的速度阈值，线速度 m/s 与角速度 rad/s 共用。
  float standstill_eps = 0.05f;

  // 多帧准备与正式启用之间的间隔，以及地形图设置后的稳定时间。
  int multiframe_prep_delay_ms = 300;
  int settle_delay_ms = 200;
};

class GaitCoordinator {
 public:
  struct Result {
    bool ok = false;
    std::string code;     // 机器可读，便于遥控端分支处理
    std::string message;  // 面向操作员
  };
  using ResultHandler = std::function<void(const Result&)>;

  GaitCoordinator(MotionClient& motion, TerrainClient& terrain,
                  GaitCoordinatorConfig config);
  ~GaitCoordinator();

  GaitCoordinator(const GaitCoordinator&) = delete;
  GaitCoordinator& operator=(const GaitCoordinator&) = delete;

  void Start();
  void Stop();

  // 异步发起切换。stair_style 仅对单帧楼梯步态有意义，其余情况忽略。
  // 已有切换在途时返回 false，不排队 —— 操作员连点两下不应该叠加执行。
  bool Request(Gait target, HeightMapMode stair_style, ResultHandler on_done,
               bool stepping_hint = false);
  // 力控/停步时选中的配置只记下；起步请求后调用，工作线程会等狗主机确认踏步再执行。
  void ApplyQueuedWhenWalking();
  void ClearQueued();

  bool busy() const { return busy_.load(); }

 private:
  void WorkerLoop();
  Result Execute(Gait target, HeightMapMode stair_style, bool stepping_hint);

  // 等待机器狗速度落到阈值以下。
  bool WaitStandstill();
  // 等待遥测里的步态变成 target。
  bool WaitGaitConfirmed(Gait target);
  bool WaitWalking();

  MotionClient& motion_;
  TerrainClient& terrain_;
  GaitCoordinatorConfig cfg_;

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> busy_{false};

  std::mutex mutex_;
  std::condition_variable cv_;
  bool pending_ = false;
  Gait pending_target_ = Gait::kWalk;
  HeightMapMode pending_style_ = HeightMapMode::kSolid;
  bool pending_step_hint_ = false;
  ResultHandler pending_handler_;
  bool queued_ = false;
  bool apply_queued_requested_ = false;
  Gait queued_target_ = Gait::kWalk;
  HeightMapMode queued_style_ = HeightMapMode::kSolid;
  ResultHandler queued_handler_;
};

}  // namespace x30
