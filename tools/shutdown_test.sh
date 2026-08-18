#!/usr/bin/env bash
# 关停耗时测试。
#
# 网关关不干净是很难发现的一类问题：本地测试都过，装到板子上就变成
# systemctl restart 卡住、看门狗超时强杀、日志断在一半。
#
# 最容易踩的两处都在点云链路上：
#   1. 阻塞在 accept() 的线程，关掉监听套接字并不能可靠唤醒它
#   2. connect() 到掉电的感知主机，内核会重传 SYN 一分多钟
# 所以这里专门用一个**不可达**的 ROS master 来测，而不是用仿真器。

set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)

GATEWAY="${1:-}"
if [[ -z "$GATEWAY" ]]; then
  for candidate in build/x30_gateway build-wsl/x30_gateway; do
    if [[ -x "$candidate" ]]; then GATEWAY="$candidate"; break; fi
  done
fi
if [[ -z "$GATEWAY" || ! -x "$GATEWAY" ]]; then
  echo "找不到网关可执行文件，请先编译。" >&2
  exit 1
fi

# 关停预算。systemd 默认 TimeoutStopSec=90s，但我们自己的目标要严得多：
# 现场重启服务不该让操作员等。
BUDGET_MS=3000

python3 "$ROOT/tools/x30_sim.py" > /tmp/x30_shutdown_sim.log 2>&1 &
SIM_PID=$!
sleep 1

# 192.0.2.0/24 是 RFC 5737 保留的测试网段，保证不可路由 ——
# 用它才能真正触发 SYN 重传那条路径。
"$GATEWAY" --robot-ip 127.0.0.1 --perception-ip 127.0.0.1 \
    --serve --port 8098 --web "$ROOT/web" --cloud \
    --ros-master http://192.0.2.1:11311 --ros-host 127.0.0.1 \
    > /tmp/x30_shutdown_gw.log 2>&1 &
GW_PID=$!

sleep 3
if ! kill -0 "$GW_PID" 2>/dev/null; then
  echo "网关没能启动："
  cat /tmp/x30_shutdown_gw.log
  kill "$SIM_PID" 2>/dev/null
  exit 1
fi

echo "发送 SIGTERM，开始计时"
START=$(date +%s%N)
kill -TERM "$GW_PID"

# 不能用 wait：超时的话它会一直等下去，测试本身就挂了。
ELAPSED_MS=0
while kill -0 "$GW_PID" 2>/dev/null; do
  sleep 0.05
  NOW=$(date +%s%N)
  ELAPSED_MS=$(( (NOW - START) / 1000000 ))
  if (( ELAPSED_MS > 20000 )); then
    echo "关停超过 20 秒，判定为卡死，强杀"
    kill -9 "$GW_PID" 2>/dev/null
    kill "$SIM_PID" 2>/dev/null
    echo "失败：关停卡死"
    exit 1
  fi
done
NOW=$(date +%s%N)
ELAPSED_MS=$(( (NOW - START) / 1000000 ))

kill "$SIM_PID" 2>/dev/null
wait 2>/dev/null

echo "关停耗时 ${ELAPSED_MS} ms（预算 ${BUDGET_MS} ms）"
if (( ELAPSED_MS > BUDGET_MS )); then
  echo "失败：关停过慢"
  exit 1
fi
echo "通过"
