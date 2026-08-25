#!/usr/bin/env bash
# 遥控服务端到端测试：拉起仿真器 + 网关，跑协议探针，收尾清理。
#
#   bash tools/serve_test.sh
#
# 需要先编译： cmake -S rk3588 -B build && cmake --build build -j

set -u

cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)

GATEWAY="$ROOT/build/x30_gateway"
if [[ ! -x "$GATEWAY" ]]; then
  echo "找不到 $GATEWAY，请先编译。"
  exit 1
fi

SIM_PID=""
GW_PID=""
ROS_PID=""
FAILED=0

cleanup() {
  [[ -n "$GW_PID" ]] && kill "$GW_PID" 2>/dev/null
  [[ -n "$SIM_PID" ]] && kill "$SIM_PID" 2>/dev/null
  [[ -n "$ROS_PID" ]] && kill "$ROS_PID" 2>/dev/null
  wait 2>/dev/null
  GW_PID=""
  SIM_PID=""
  ROS_PID=""
}
trap cleanup EXIT

# 等某个后台进程真的就绪，而不是固定 sleep 几秒。
#
# 固定等待在机器忙的时候不够用：Python 起得慢一点，网关早期发出去的指令
# 就全打空了。现象是一大片断言同时失败，单独重跑却全是好的 —— 最难查的那种。
# 所以一律等它自己说"我起来了"。
#
# 配合 python3 -u 使用：这些脚本的 print 在管道下是块缓冲的，
# 不关缓冲的话日志要攒够 4KB 才落盘，等于白等。
wait_ready() {
  local log=$1 mark=$2 what=$3
  for _ in $(seq 100); do
    grep -q "$mark" "$log" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "$what 10 秒内没能就绪，它的输出："
  cat "$log"
  return 1
}

# run_scenario <标题> <探针场景> <日志后缀> [网关的额外参数...]
#
# 每个场景都用全新的仿真器和网关，避免上一个场景的残留状态影响下一个 ——
# 步态、控制权、媒体槽位都是有状态的，复用进程会让失败很难定位。
run_scenario() {
  local title=$1 probe=$2 tag=$3
  shift 3

  echo
  echo "== 场景：$title =="
  python3 -u "$ROOT/tools/x30_sim.py" > "/tmp/x30_sim_$tag.log" 2>&1 &
  SIM_PID=$!
  if ! wait_ready "/tmp/x30_sim_$tag.log" "仿真器已启动" "仿真器"; then
    FAILED=1; cleanup; return
  fi

  # 只听回环。默认监听 0.0.0.0 时，局域网里真在跑的平板会连上测试网关（它每两秒
  # 重连一次同一个端口），抢走全码率槽位，于是媒体编排那组断言莫名失败 ——
  # 排查这种"失败"要花掉一整个下午，而它跟被测代码毫无关系。
  "$GATEWAY" --robot-ip 127.0.0.1 --perception-ip 127.0.0.1 \
      --serve --bind 127.0.0.1 --web "$ROOT/web" "$@" \
      > "/tmp/x30_gw_$tag.log" 2>&1 &
  GW_PID=$!
  if ! wait_ready "/tmp/x30_gw_$tag.log" "遥控服务已就绪" "网关"; then
    FAILED=1; cleanup; return
  fi

  if ! kill -0 "$GW_PID" 2>/dev/null; then
    echo "网关启动失败："
    cat "/tmp/x30_gw_$tag.log"
    FAILED=1
    cleanup
    return
  fi

  if ! python3 "$ROOT/tools/ws_probe.py" --host 127.0.0.1 --port 8080 \
      --scenario "$probe"; then
    FAILED=1
  fi

  cleanup
}

# 基线：完整协议走一遍。
run_scenario "完整协议" full base

# 主机可达但地形图端口没人监听，模拟感知主机上模块没起来。只改端口这一个变量，
# 确保失败确实来自地形图通道而不是别的原因。楼梯步态必须如实报错 ——
# 这是实机上最难查的一类故障，值得单独兜住。
run_scenario "地形图模块未启动" no-terrain noterrain --perception-port 43555

# 媒体编排：能力协商、码流降级、全码率槽位仲裁。不需要真视频，
# 网关本来就只下发计划、不搬运字节。
run_scenario "媒体编排" media media --media "$ROOT/deploy/media.json"

# 没有视频时控制必须完全不受影响 —— 视频是附加能力，不该拖垮本体。
run_scenario "未配置媒体源" no-media nomedia

# 点云：需要额外拉一个假 ROS master，所以不套 run_scenario。
# 验的是 XML-RPC 发现、TCPROS 握手、解析、降采样、量化下行这一整条链，
# 这些在现场没法调试 —— 感知主机是机器狗的一部分，出问题只能干等。
echo
echo "== 场景：点云下行 =="
python3 -u "$ROOT/tools/ros_sim.py" --port 11400 --points 60000 \
    > /tmp/x30_rossim.log 2>&1 &
ROS_PID=$!
python3 -u "$ROOT/tools/x30_sim.py" > /tmp/x30_sim_cloud.log 2>&1 &
SIM_PID=$!
wait_ready /tmp/x30_rossim.log "Ctrl-C 退出" "假 ROS master" || FAILED=1
wait_ready /tmp/x30_sim_cloud.log "仿真器已启动" "仿真器" || FAILED=1

"$GATEWAY" --robot-ip 127.0.0.1 --perception-ip 127.0.0.1 \
    --serve --web "$ROOT/web" --cloud \
    --ros-master http://127.0.0.1:11400 --ros-host 127.0.0.1 \
    > /tmp/x30_gw_cloud.log 2>&1 &
GW_PID=$!
wait_ready /tmp/x30_gw_cloud.log "遥控服务已就绪" "网关" || FAILED=1

if ! kill -0 "$GW_PID" 2>/dev/null; then
  echo "网关启动失败："
  cat /tmp/x30_gw_cloud.log
  FAILED=1
else
  if ! python3 "$ROOT/tools/cloud_probe.py" --host 127.0.0.1 --port 8080; then
    FAILED=1
  fi
fi
cleanup

# 感知主机不可达：点云必须如实报错，且控制完全不受影响。
# 这是现场最可能遇到的情况 —— ROS 可达性至今没验证过。
echo
echo "== 场景：感知主机不可达 =="
python3 -u "$ROOT/tools/x30_sim.py" > /tmp/x30_sim_nocloud.log 2>&1 &
SIM_PID=$!
wait_ready /tmp/x30_sim_nocloud.log "仿真器已启动" "仿真器" || FAILED=1
"$GATEWAY" --robot-ip 127.0.0.1 --perception-ip 127.0.0.1 \
    --serve --web "$ROOT/web" --cloud \
    --ros-master http://127.0.0.1:11999 --ros-host 127.0.0.1 \
    > /tmp/x30_gw_nocloud.log 2>&1 &
GW_PID=$!
wait_ready /tmp/x30_gw_nocloud.log "遥控服务已就绪" "网关" || FAILED=1
# 必须确认起来的是我们这个网关。少了这一步，端口上如果残留着别的实例，
# 探针会连过去并给出看似正常的结果 —— 这种假通过比失败更糟。
if ! kill -0 "$GW_PID" 2>/dev/null; then
  echo "网关启动失败："
  cat /tmp/x30_gw_nocloud.log
  FAILED=1
elif ! python3 "$ROOT/tools/ws_probe.py" --host 127.0.0.1 --port 8080 \
    --scenario cloud-down; then
  FAILED=1
fi
cleanup

echo
echo "---- 网关日志（完整协议场景）----"
cat /tmp/x30_gw_base.log

exit $FAILED
