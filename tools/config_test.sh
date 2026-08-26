#!/usr/bin/env bash
# 在线改配置的端到端测试：拉起仿真器 + 网关，验令牌、校验、落盘、自重启。
#
#   bash tools/config_test.sh
#
# 需要先编译： cmake -S rk3588 -B build && cmake --build build -j
#
# 为什么单独一个脚本：这一组要反复换配置文件、还要验网关"改完自己退出"，
# 与 serve_test.sh 里那些"起一次跑一遍"的场景在生命周期上是相反的。

set -u

cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)

GATEWAY="$ROOT/build/x30_gateway"
if [[ ! -x "$GATEWAY" ]]; then
  echo "找不到 $GATEWAY，请先编译。"
  exit 1
fi

# shellcheck source=deploy/config_util.sh
source "$ROOT/deploy/config_util.sh"

# 端口现挑，别用默认值：装好的那份服务占着 8080 和 43897，另一轮测试在跑时
# 还会再撞一次。原因见 ports.sh。
# shellcheck source=tools/ports.sh
source "$ROOT/tools/ports.sh"
PORT=$(free_port tcp)
SIM_PORT=$(free_port udp)
TELEM_PORT=$(free_port udp)
TERRAIN_PORT=$(free_port udp)
TMP=$(mktemp -d)
CONF="$TMP/conf/gateway.conf"
TOKEN_FILE="$TMP/conf/admin.token"
FAILED=0
SIM_PID=""
GW_PID=""

pass() { echo "  [OK]   $1"; }
fail() { echo "  [FAIL] $1${2:+  $2}"; FAILED=1; }

cleanup() {
  [[ -n "$GW_PID" ]] && kill "$GW_PID" 2>/dev/null
  [[ -n "$SIM_PID" ]] && kill "$SIM_PID" 2>/dev/null
  wait 2>/dev/null
  GW_PID=""
  SIM_PID=""
}
trap 'cleanup; rm -rf "$TMP"' EXIT

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

# 造一份装好的样子。地址一律指回本机：这组测试验的是配置通路，
# 不需要真的连上机器狗。
conf_defaults
ROBOT_IP="127.0.0.1"
PERCEPTION_IP="127.0.0.2"
ROBOT_PORT="$SIM_PORT"
LOCAL_PORT="$TELEM_PORT"
PERCEPTION_PORT="$TERRAIN_PORT"
HTTP_PORT="$PORT"
BIND_ADDR="127.0.0.1"
write_gateway_conf "$CONF"
TOKEN="54longqr"

start_gateway() {   # start_gateway <日志> [额外环境…]
  local log=$1
  python3 -u "$ROOT/tools/x30_sim.py" --listen-port "$SIM_PORT" \
      --terrain-port "$TERRAIN_PORT" --target "127.0.0.1:$TELEM_PORT" \
      > "$TMP/sim.log" 2>&1 &
  SIM_PID=$!
  wait_ready "$TMP/sim.log" "仿真器已启动" "仿真器" || return 1

  "$GATEWAY" --config "$CONF" --admin-token-file "$TOKEN_FILE" \
      --serve --web "$ROOT/web" > "$log" 2>&1 &
  GW_PID=$!
  wait_ready "$log" "遥控服务已就绪" "网关" || return 1
  # 必须确认起来的是我们这个网关。端口上残留着别的实例时，探针会连过去
  # 并给出看似正常的结果 —— 这种假通过比失败更糟。
  kill -0 "$GW_PID" 2>/dev/null
}

# ---------------------------------------------------------------------------
echo "== 配置文件驱动的启动 =="
# ---------------------------------------------------------------------------

if ! start_gateway "$TMP/gw.log"; then
  fail "网关起不来" "$(tail -5 "$TMP/gw.log")"
  exit 1
fi
pass "网关按配置文件启动"

# 配置文件里写的地址必须真的被用上。只验"进程活着"是不够的 ——
# 参数没接上时它照样活着，只是连错了地方。
if grep -q "已连接 127.0.0.1:$SIM_PORT" "$TMP/gw.log"; then
  pass "运动主机地址取自配置文件"
else
  fail "运动主机地址没有取自配置文件" "$(head -3 "$TMP/gw.log")"
fi
if grep -q "地形图通道 127.0.0.2" "$TMP/gw.log"; then
  pass "感知主机地址取自配置文件"
else
  fail "感知主机地址没有取自配置文件" "$(head -3 "$TMP/gw.log")"
fi
if grep -q "监听地址 127.0.0.1" "$TMP/gw.log"; then
  pass "监听地址取自配置文件"
else
  fail "监听地址没有取自配置文件" "$(head -6 "$TMP/gw.log")"
fi

# ---------------------------------------------------------------------------
echo
echo "== 协议层（令牌、校验、互锁、落盘）=="
# ---------------------------------------------------------------------------

if ! python3 "$ROOT/tools/ws_probe.py" --host 127.0.0.1 --port "$PORT" \
    --scenario config --token "$TOKEN" --conf "$CONF"; then
  FAILED=1
fi

# 探针改过配置了，重启一次确认那份文件网关自己认得。
# "存下来了"和"下次能起来"是两件事，后者才是操作员真正依赖的。
cleanup
echo
echo "== 改完之后必须还能起来 =="
if start_gateway "$TMP/gw2.log"; then
  pass "带着改后的配置重新启动成功"
  if grep -q "已连接 192.168.1.203:$SIM_PORT" "$TMP/gw2.log"; then
    pass "重启后用的是新地址"
  else
    fail "重启后没用新地址" "$(head -3 "$TMP/gw2.log")"
  fi
else
  fail "带着改后的配置起不来" "$(tail -5 "$TMP/gw2.log")"
fi
cleanup

# ---------------------------------------------------------------------------
echo
echo "== 密码不对时一律拒绝 =="
# ---------------------------------------------------------------------------

# 密码不对必须拒，不能谁都能改监听地址。
conf_defaults
ROBOT_IP="127.0.0.1"
PERCEPTION_IP="127.0.0.2"
ROBOT_PORT="$SIM_PORT"
LOCAL_PORT="$TELEM_PORT"
PERCEPTION_PORT="$TERRAIN_PORT"
HTTP_PORT="$PORT"
BIND_ADDR="127.0.0.1"
write_gateway_conf "$CONF"
rm -f "$TOKEN_FILE"

if start_gateway "$TMP/gw3.log"; then
  OUT=$(python3 - "$PORT" "wrong-password" <<'PY'
import sys, os
sys.path.insert(0, os.path.join(os.getcwd(), "tools"))
from ws_probe import WsClient
port, token = int(sys.argv[1]), sys.argv[2]
c = WsClient("127.0.0.1", port)
c.wait_for("hello")
c.send({"t": "config_set", "token": token,
        "settings": {"robot_ip": "10.0.0.1"}})
print(c.wait_for("error", timeout=5).get("code", ""))
c.close()
PY
)
  if [[ "$OUT" == "bad_admin_token" ]]; then
    pass "密码不对时改配置被拒（bad_admin_token）"
  else
    fail "密码不对时的回应不对" "实得: $OUT"
  fi
  if grep -q "robot_ip = 127.0.0.1" "$CONF"; then
    pass "被拒之后文件没被动过"
  else
    fail "被拒了却改了文件" "$(grep robot_ip "$CONF")"
  fi
else
  fail "密码校验那段网关起不来" "密码错只该拒改配置，不该拖垮服务"
fi
cleanup

# ---------------------------------------------------------------------------
echo
echo "== systemd 托管时才自己退出重启 =="
# ---------------------------------------------------------------------------

# 手工在终端里跑的话，退出就真的没了 —— 上面那组已经验过它不退。
# 由 systemd 托管时相反：必须干净退出，让 Restart=always 带着新配置把它拉回来。
# 用 INVOCATION_ID 判断，那是 systemd 给每个服务实例设的环境变量。
TOKEN="54longqr"

python3 -u "$ROOT/tools/x30_sim.py" --listen-port "$SIM_PORT" \
    --terrain-port "$TERRAIN_PORT" --target "127.0.0.1:$TELEM_PORT" \
    > "$TMP/sim.log" 2>&1 &
SIM_PID=$!
wait_ready "$TMP/sim.log" "仿真器已启动" "仿真器" || FAILED=1

INVOCATION_ID=deadbeefdeadbeefdeadbeefdeadbeef \
  "$GATEWAY" --config "$CONF" --admin-token-file "$TOKEN_FILE" \
    --serve --web "$ROOT/web" > "$TMP/gw4.log" 2>&1 &
GW_PID=$!
if wait_ready "$TMP/gw4.log" "遥控服务已就绪" "网关"; then
  OUT=$(python3 - "$PORT" "$TOKEN" <<'PY'
import sys, os
sys.path.insert(0, os.path.join(os.getcwd(), "tools"))
from ws_probe import WsClient
port, token = int(sys.argv[1]), sys.argv[2]
c = WsClient("127.0.0.1", port)
c.wait_for("hello")
c.send({"t": "config_get", "token": token})
cfg = c.wait_for("config", timeout=5)
print("auto_restart=%s" % cfg.get("auto_restart"))
c.send({"t": "config_set", "token": token,
        "settings": {"robot_ip": "192.168.1.111"}})
print("saved=%s" % c.wait_for("config_saved", timeout=5)
      .get("auto_restart"))
c.close()
PY
)
  if [[ "$OUT" == *"auto_restart=True"* ]]; then
    pass "systemd 环境下承诺自动重启"
  else
    fail "systemd 环境下没承诺自动重启" "实得: $OUT"
  fi

  # 回执发出后网关应当在一秒内干净退出。等它，不要立刻断言 ——
  # 那 400 毫秒的延迟是故意留给回执出门的。
  GONE=0
  for _ in $(seq 30); do
    kill -0 "$GW_PID" 2>/dev/null || { GONE=1; break; }
    sleep 0.1
  done
  if [[ $GONE -eq 1 ]]; then
    pass "改完配置后干净退出，交给 systemd 拉回来"
  else
    fail "改完配置后没有退出" "systemd 下新配置不会生效"
  fi

  if grep -q "配置已更新，正在重启以生效" "$TMP/gw4.log"; then
    pass "日志里说清了这次退出是为了应用新配置"
  else
    fail "日志没说明退出原因" "$(tail -3 "$TMP/gw4.log")"
  fi

  # 走的是干净关停那条路：轴指令先归零，再收摊。直接 _exit 的话，
  # 机器狗会带着最后一次速度一直走到自己的看门狗超时。
  if grep -q "正在停止\|重启以生效" "$TMP/gw4.log"; then
    pass "退出前走了干净关停（先归零轴指令）"
  else
    fail "没看到干净关停的迹象" "$(tail -3 "$TMP/gw4.log")"
  fi

  GW_PID=""   # 已经自己退了，cleanup 不用再管它
  if grep -q "robot_ip = 192.168.1.111" "$CONF"; then
    pass "新配置已落盘"
  else
    fail "新配置没落盘" "$(grep robot_ip "$CONF")"
  fi
else
  fail "网关起不来" "$(tail -5 "$TMP/gw4.log")"
fi
cleanup

echo
if [[ $FAILED -eq 0 ]]; then
  echo "在线改配置检查通过"
else
  echo "在线改配置检查失败"
fi
exit $FAILED
