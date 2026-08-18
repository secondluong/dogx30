#!/usr/bin/env bash
# 验证 --bind 真的把服务限制在了指定网卡上。
#
# 绑到回环地址后，从本机其他网卡地址应当连不上。这条性质是 4G 在网时
# 遥控端口不被暴露到广域网的依据，值得单独验一次。

set -u
cd "$(dirname "$0")/.." || exit 1

BIN=./build/x30_gateway
if [ ! -x "$BIN" ] || [ ! -f tools/x30_sim.py ]; then
  echo "先编译网关，并确认 tools/x30_sim.py 存在"
  exit 1
fi

cleanup() {
  [ -n "${GW_PID:-}" ] && kill "$GW_PID" 2>/dev/null
  [ -n "${SIM_PID:-}" ] && kill "$SIM_PID" 2>/dev/null
  wait 2>/dev/null
}
trap cleanup EXIT

python3 tools/x30_sim.py >/tmp/bindchk-sim.log 2>&1 &
SIM_PID=$!
sleep 1

"$BIN" --robot-ip 127.0.0.1 --serve --web web --bind 127.0.0.1 \
    >/tmp/bindchk-gw.log 2>&1 &
GW_PID=$!
sleep 2

LAN=$(hostname -I | tr ' ' '\n' | grep -v '^$' | head -1)
echo "网关监听: $(grep -a 监听地址 /tmp/bindchk-gw.log)"
echo "本机另一地址: $LAN"
echo

fail=0

# curl 连不上时 -w 已经会输出 000，不要再用 || echo 兜底，否则会拼成 000000。
probe() { curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$1"; }

code=$(probe http://127.0.0.1:8080/)
if [ "$code" = "200" ]; then
  echo "  [PASS] 127.0.0.1:8080 可访问（遥控链路正常）"
else
  echo "  [FAIL] 127.0.0.1:8080 应当可访问，实际 HTTP $code"
  fail=1
fi

if [ -z "$LAN" ] || [ "$LAN" = "127.0.0.1" ]; then
  echo "  [SKIP] 本机没有第二个地址，无法验证隔离性"
else
  code=$(probe "http://$LAN:8080/")
  if [ "$code" = "000" ]; then
    echo "  [PASS] $LAN:8080 连不上（未绑定的网卡确实不监听）"
  else
    echo "  [FAIL] $LAN:8080 竟然返回 HTTP $code，--bind 没有生效"
    fail=1
  fi
fi

echo
[ "$fail" = 0 ] && echo "全部通过" || echo "存在失败项"
exit "$fail"
