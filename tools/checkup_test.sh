#!/usr/bin/env bash
# 验 deploy/checkup.sh 能把 install.sh 生成的单元文件正确读回来。
#
# 这两个脚本是隔空配合的：install.sh 用 sed 拼出 ExecStart，checkup.sh 再把它
# 拆回来。改了一边忘了另一边的话，体检不会报错，只会**安静地报着默认地址** ——
# 那比直接失败糟得多，因为人会信它。

set -u
cd "$(dirname "$0")/.." || exit 1

FAILED=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass() { echo "  [OK]   $1"; }
fail() { echo "  [FAIL] $1  $2"; FAILED=1; }

# 用 install.sh 真正在用的那个函数，而不是照着抄一份
# shellcheck source=deploy/render_unit.sh
source deploy/render_unit.sh

render() {   # render <bind> <perception> <cloud> <ros_host> <robot> <port>
  render_unit deploy/x30-gateway.service \
    "$5" "$2" 43897 "$6" "$1" "$3" "$4" /opt/x30
}

# 只要"安装配置"那一段 —— 后面的检查要连真设备，在这里跑纯属浪费。
config_line() {
  X30_UNIT="$1" bash deploy/checkup.sh --config-only 2>/dev/null |
    grep -E '运动主机|服务端口' | tr -s ' '
}

echo "== 读回 install.sh 生成的配置 =="

render 192.168.10.2 192.168.1.200 yes 192.168.1.120 192.168.1.106 9090 \
  > "$TMP/a.service"
OUT=$(config_line "$TMP/a.service")

for want in "运动主机 192.168.1.106" "感知主机 192.168.1.200" \
            "服务端口 9090" "监听 192.168.10.2" "点云 yes"; do
  if [[ "$OUT" == *"$want"* ]]; then
    pass "读到 $want"
  else
    fail "没读到 $want" "实得: $OUT"
  fi
done

echo
echo "== 默认安装（点云关闭）=="

render 0.0.0.0 192.168.1.105 no 192.168.1.120 192.168.1.103 8080 \
  > "$TMP/b.service"
OUT=$(config_line "$TMP/b.service")

for want in "运动主机 192.168.1.103" "感知主机 192.168.1.105" \
            "服务端口 8080" "监听 0.0.0.0" "点云 no"; do
  if [[ "$OUT" == *"$want"* ]]; then
    pass "读到 $want"
  else
    fail "没读到 $want" "实得: $OUT"
  fi
done

echo
echo "== 没装的时候要说人话 =="

OUT=$(X30_UNIT="$TMP/nonexistent.service" bash deploy/checkup.sh 2>&1)
RC=$?
if [[ $RC -ne 0 ]]; then
  pass "未安装时返回非零"
else
  fail "未安装时返回了 0" "应当报错退出"
fi
if [[ "$OUT" == *"install.sh"* ]]; then
  pass "提示了该怎么装"
else
  fail "没提示该怎么装" "$OUT"
fi

echo
echo "== 单元文件坏了要报错，不能退回默认值 =="

printf '[Service]\nExecStart=\n' > "$TMP/broken.service"
OUT=$(X30_UNIT="$TMP/broken.service" bash deploy/checkup.sh --config-only 2>&1)
if [[ "$OUT" == *"192.168.1.103"* ]]; then
  fail "读不出参数时谎报了默认地址" "$OUT"
else
  pass "没有拿默认值冒充真实配置"
fi

echo
echo "== 多行 ExecStart 必须整段解析 =="
# 这一条专门盯着那个真实出过的问题：只 grep 第一行的话，
# 所有参数都抠不到，然后静静地退回默认值。
FIRST=$(grep '^ExecStart=' "$TMP/a.service")
if [[ "$FIRST" == *"--robot-ip"* ]]; then
  fail "夹具不对" "ExecStart 第一行就含参数，这条测试失去意义"
else
  pass "ExecStart 首行确实不含参数（续行结构没变）"
fi

echo
if [[ $FAILED -eq 0 ]]; then
  echo "checkup 配置解析检查通过"
else
  echo "checkup 配置解析检查失败"
fi
exit $FAILED
