#!/usr/bin/env bash
# 端到端冒烟测试：拉起仿真器，用网关跑一遍完整的起立-踏步-行走-停止流程，
# 校验遥测解析结果是否符合预期。无需实机。
#
# 用法: bash tools/smoke_test.sh [网关可执行文件路径]

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

# 开发机上习惯编到 build-wsl/，板子上 install.sh 编到 build/，两处都找一下。
GATEWAY="${1:-}"
if [[ -z "$GATEWAY" ]]; then
  for candidate in build-wsl/x30_gateway build/x30_gateway; do
    if [[ -x "$candidate" ]]; then GATEWAY="$candidate"; break; fi
  done
fi
if [[ -z "$GATEWAY" || ! -x "$GATEWAY" ]]; then
  echo "找不到网关可执行文件，请先编译，或用参数指定路径。" >&2
  echo "  cmake -S rk3588 -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)" >&2
  exit 1
fi

# 端口一律现挑，别用默认的 43893/43897：板子上装好的那份服务占着 43897，
# 撞上之后遥测会被分走一半，急停和看门狗这些断言随机红。原因见 ports.sh。
# shellcheck source=tools/ports.sh
source "$(dirname "$0")/ports.sh"
ROBOT_PORT=$(free_port udp)
LOCAL_PORT=$(free_port udp)
TERRAIN_PORT=$(free_port udp)

SIM_LOG=$(mktemp)
GW_LOG=$(mktemp)
trap 'rm -f "$SIM_LOG" "$GW_LOG"' EXIT

python3 -u tools/x30_sim.py --listen-port "$ROBOT_PORT" --terrain-port "$TERRAIN_PORT" \
    --target "127.0.0.1:$LOCAL_PORT" > "$SIM_LOG" 2>&1 &
SIM_PID=$!
trap 'kill $SIM_PID 2>/dev/null; rm -f "$SIM_LOG" "$GW_LOG"' EXIT

# 等仿真器真的把端口绑上，而不是固定 sleep 一秒。
#
# 固定等待在机器忙的时候不够用 —— Python 解释器起来慢一点，网关早期发的
# stand/torque 就全打空了。现象是一大片断言同时失败，而单独重跑又全是好的，
# 属于最难查的那一类。这个坑真踩过一次。
#
# 仿真器绑完端口才打印这一行（flush=True），等它比查端口更直接，
# 也不依赖 ss / netstat 是否存在。
for _ in $(seq 100); do
  grep -q "仿真器已启动" "$SIM_LOG" 2>/dev/null && break
  kill -0 $SIM_PID 2>/dev/null || break
  sleep 0.1
done
if ! grep -q "仿真器已启动" "$SIM_LOG" 2>/dev/null; then
  echo "仿真器 10 秒内没能就绪，它的输出：" >&2
  cat "$SIM_LOG" >&2
  exit 1
fi

# 用 stdin 喂命令，sleep 用来等状态机迁移完成（起立/坐下各需 2 秒）
{
  sleep 1                        # 等 RX 线程收到第一帧遥测
  echo "s";        sleep 1
  echo "stand";    sleep 3
  echo "torque";   sleep 1
  echo "step";     sleep 1
  echo "gait walk"; sleep 1
  echo "v 0.5 0 0.2"; sleep 3
  echo "s"
  echo "stop";     sleep 2
  echo "s"
  # 看门狗：重新加速，然后停止喂数据但不主动清零，
  # 机器人必须靠 MotionClient 的超时自己停下来
  echo "v 0.5 0 0"; sleep 3
  echo "s"
  echo "freeze";   sleep 2
  echo "s"
  echo "estop";    sleep 1
  echo "s"
  echo "unload";   sleep 1
  echo "q"
} | "$GATEWAY" --robot-ip 127.0.0.1 --robot-port "$ROBOT_PORT" \
      --local-port "$LOCAL_PORT" --interactive > "$GW_LOG" 2>&1

kill $SIM_PID 2>/dev/null
wait $SIM_PID 2>/dev/null

echo "================ 仿真器侧 ================"
cat "$SIM_LOG"
echo "================ 网关侧 =================="
grep -E "状态|里程计|速度|姿态|电池|温度|告警|急停|遥测" "$GW_LOG"

echo "================ 断言 ===================="
fail=0
check() {
  if grep -qE "$2" "$1"; then
    echo "  PASS  $3"
  else
    echo "  FAIL  $3"
    fail=1
  fi
}

# 看门狗断言：倒数第二次 s 之前发了 freeze，此时速度必须已经归零。
# 取全部 vx 读数，最后两条应分别是 freeze 前的 0.6x 和 freeze 后的 0.00。
mapfile -t VX < <(grep -oE "vx=[-0-9.]+" "$GW_LOG" | sed 's/vx=//')
n=${#VX[@]}
if (( n >= 3 )); then
  before_freeze="${VX[n-3]}"
  after_freeze="${VX[n-2]}"
else
  before_freeze="?" ; after_freeze="?"
fi

check "$SIM_LOG" "收到连接确认"          "连接确认已送达（心跳后必须补发）"
check "$SIM_LOG" "RL起立中"              "起立走原厂 RL 路径（0x21010223），不是旧切换"
check "$SIM_LOG" "进入「初始站立」"       "站立指令被正确解析"
if grep -qE "起立中" "$SIM_LOG" && ! grep -qE "RL起立中" "$SIM_LOG"; then
  echo "  FAIL  起立还在走旧的 0x21010202，轨迹会又快又硬"
  fail=1
fi
check "$SIM_LOG" "进入力控站立"          "力控切换被正确解析"
check "$SIM_LOG" "开始踏步"              "起步指令被正确解析"
check "$SIM_LOG" "软急停"                "软急停被正确解析"
check "$SIM_LOG" "卸力"                  "急停后卸力走 0x21010202，解除关节保护"
check "$GW_LOG"  "状态 +踏步"            "网关解析出踏步状态"
check "$GW_LOG"  "vx=0\.[3-9]"           "速度指令闭环回传（vx 应接近 0.5*1.2=0.6）"
check "$GW_LOG"  "电池 +80%"             "电池遥测解析正确"
check "$GW_LOG"  "状态 +急停/跌倒"        "急停后状态回传正确"

if [[ "$before_freeze" != "?" ]] \
   && awk "BEGIN{exit !($before_freeze > 0.3)}" \
   && awk "BEGIN{exit !($after_freeze < 0.05)}"; then
  echo "  PASS  看门狗超时后自动停车（vx $before_freeze -> $after_freeze）"
else
  echo "  FAIL  看门狗未生效（vx $before_freeze -> $after_freeze，期望 >0.3 变 <0.05）"
  fail=1
fi

if ! grep -qE "未识别的指令码" "$SIM_LOG"; then
  echo "  PASS  没有发出仿真器无法识别的指令码"
else
  echo "  FAIL  出现未识别的指令码:"
  grep "未识别的指令码" "$SIM_LOG" | sort -u | sed 's/^/          /'
  fail=1
fi

# 起立/坐下过程中发轴（尤其是身高=0）会把原厂柔和轨迹掐成猛起猛趴。
if ! grep -qE "过渡态收到轴指令|过渡期间共收到轴指令" "$SIM_LOG"; then
  echo "  PASS  起立/坐下过渡期间没有发轴指令"
else
  echo "  FAIL  过渡期间仍在发轴，起立/趴下会被掐得又快又硬"
  grep -E "过渡态收到轴指令|过渡期间共收到轴指令" "$SIM_LOG" | sed 's/^/          /'
  fail=1
fi

echo
echo "================ RL 遥测撒谎后仍能趴下 ===================="
# 实机 RL 起立后 basic_state 仍报 0。若网关信遥测，第二次「坐/站」会再发起立。
LIE_SIM=$(mktemp)
LIE_GW=$(mktemp)
python3 -u tools/x30_sim.py --listen-port "$ROBOT_PORT" --terrain-port "$TERRAIN_PORT" \
    --target "127.0.0.1:$LOCAL_PORT" --lie-rl-state > "$LIE_SIM" 2>&1 &
LIE_PID=$!
for _ in $(seq 100); do
  grep -q "仿真器已启动" "$LIE_SIM" 2>/dev/null && break
  kill -0 $LIE_PID 2>/dev/null || break
  sleep 0.1
done
{
  sleep 1
  echo "stand"; sleep 3
  echo "stand"; sleep 3
  echo "q"
} | "$GATEWAY" --robot-ip 127.0.0.1 --robot-port "$ROBOT_PORT" \
      --local-port "$LOCAL_PORT" --interactive > "$LIE_GW" 2>&1
kill $LIE_PID 2>/dev/null
wait $LIE_PID 2>/dev/null
if grep -q "RL趴下中" "$LIE_SIM"; then
  echo "  PASS  遥测仍报坐下时，第二次坐/站发出了 RL 趴下"
else
  echo "  FAIL  遥测撒谎后第二次还在起立，坐不下来"
  grep -E "RL起立|RL趴下|忽略站坐" "$LIE_SIM" | sed 's/^/          /'
  fail=1
fi
rm -f "$LIE_SIM" "$LIE_GW"

echo
LOC_TEST=""
for candidate in build/x30_localizer_test build-wsl/x30_localizer_test; do
  if [[ -x "$candidate" ]]; then LOC_TEST="$candidate"; break; fi
done
if [[ -n "$LOC_TEST" ]]; then
  if "$LOC_TEST"; then
    echo "  PASS  扫描定位走廊平移自测"
  else
    echo "  FAIL  扫描定位走廊平移自测"
    fail=1
  fi
fi

echo
if [[ $fail -eq 0 ]]; then
  echo "全部通过"
else
  echo "存在失败项"
fi
exit $fail
