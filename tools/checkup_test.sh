#!/usr/bin/env bash
# 验 deploy/checkup.sh 能把 install.sh 写下的配置正确读回来。
#
# 这两个脚本是隔空配合的：install.sh 写 gateway.conf 并让单元指向它，
# checkup.sh 再顺着单元把值读出来。改了一边忘了另一边的话，体检不会报错，
# 只会**安静地报着默认地址** —— 那比直接失败糟得多，因为人会信它。

set -u
cd "$(dirname "$0")/.." || exit 1

FAILED=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass() { echo "  [OK]   $1"; }
fail() { echo "  [FAIL] $1  $2"; FAILED=1; }

# 用 install.sh 真正在用的那两个函数，而不是照着抄一份
# shellcheck source=deploy/render_unit.sh
source deploy/render_unit.sh
# shellcheck source=deploy/config_util.sh
source deploy/config_util.sh

# 造一套装好的样子：前缀 $TMP/opt，单元指向 $TMP/opt/conf/gateway.conf
PREFIX="$TMP/opt"
CONF="$PREFIX/conf/gateway.conf"
TOKEN="$PREFIX/conf/admin.token"
UNIT="$TMP/x30.service"
render_unit deploy/x30-gateway.service "$PREFIX" > "$UNIT"

# 只要"安装配置"那一段 —— 后面的检查要连真设备，在这里跑纯属浪费。
config_line() {
  X30_UNIT="$UNIT" bash deploy/checkup.sh --config-only 2>/dev/null |
    grep -E '运动主机|服务端口|配置文件' | tr -s ' '
}

echo "== 读回 install.sh 写下的配置 =="

conf_defaults
ROBOT_IP="192.168.1.106"
PERCEPTION_IP="192.168.1.200"
HTTP_PORT="9090"
BIND_ADDR="192.168.10.2"
CLOUD="yes"
write_gateway_conf "$CONF"

OUT=$(config_line)
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

conf_defaults
write_gateway_conf "$CONF"

OUT=$(config_line)
for want in "运动主机 192.168.1.103" "感知主机 192.168.1.105" \
            "服务端口 8080" "监听 0.0.0.0" "点云 no"; do
  if [[ "$OUT" == *"$want"* ]]; then
    pass "读到 $want"
  else
    fail "没读到 $want" "实得: $OUT"
  fi
done

echo
echo "== 每一个参数都要能读回来 =="

# 上面两组只覆盖了常改的那几项。剩下的读不回来同样危险，而且更不容易发现。
conf_defaults
ROBOT_PORT="43894"
LOCAL_PORT="43898"
PERCEPTION_PORT="43900"
CLOUD="yes"
ROS_MASTER="http://10.1.2.3:11400"
ROS_HOST="10.9.9.9"
CLOUD_TOPIC="/points_raw"
CLOUD_HZ="7"
CLOUD_POINTS="31000"
PTZ_VIS_RTSP="rtsp://admin:pw@192.168.10.12:554/Streaming/Channels/101"
PTZ_IR_RTSP="rtsp://admin:pw@192.168.10.12:554/Streaming/Channels/201"
write_gateway_conf "$CONF"

check_get() {   # check_get <键> <期望值>
  local got
  got=$(conf_get "$CONF" "$1")
  if [[ $got == "$2" ]]; then
    pass "$1 = $got"
  else
    fail "$1 读回的是 $got" "应为 $2"
  fi
}
check_get robot_port 43894
check_get local_port 43898
check_get perception_port 43900
check_get ros_master "http://10.1.2.3:11400"
check_get ros_host 10.9.9.9
check_get cloud_topic /points_raw
check_get cloud_hz 7
check_get cloud_points 31000
check_get ptz_vis_rtsp "rtsp://admin:pw@192.168.10.12:554/Streaming/Channels/101"
check_get ptz_ir_rtsp "rtsp://admin:pw@192.168.10.12:554/Streaming/Channels/201"

echo
echo "== 注释和空行不能干扰解析 =="

cat > "$CONF" <<'EOF'
# 这是注释
   # 缩进的注释

robot_ip = 192.168.1.77
   perception_ip   =   192.168.1.88
# robot_ip = 192.168.1.99   ← 被注释掉的不算
EOF
OUT=$(config_line)
if [[ "$OUT" == *"运动主机 192.168.1.77"* && "$OUT" == *"感知主机 192.168.1.88"* ]]; then
  pass "注释、缩进、键值两侧空格都处理正确"
else
  fail "带注释的配置读错了" "实得: $OUT"
fi
if [[ "$OUT" == *"192.168.1.99"* ]]; then
  fail "把注释掉的那行也读进去了" "实得: $OUT"
else
  pass "注释掉的行没被当成配置"
fi

echo
echo "== 设置密码 =="

GOT=$(X30_UNIT="$UNIT" bash deploy/checkup.sh --token 2>&1)
if [[ "$GOT" == "54longqr" ]]; then
  pass "--token 打印设置密码"
else
  fail "--token 没能打印密码" "实得: $GOT"
fi

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
echo "== 配置文件丢了要报出来 =="

# 单元指着一个不存在的配置文件时，网关会用内置默认值跑起来。所以这里报
# 默认地址是**对的**，但必须同时标成失败 —— 否则人会以为那就是装的时候定的。
rm -f "$CONF"
OUT=$(X30_UNIT="$UNIT" bash deploy/checkup.sh --config-only 2>&1)
if [[ "$OUT" == *"[失败]"* ]]; then
  pass "配置文件缺失被标成失败"
else
  fail "配置文件缺失没有报错" "$OUT"
fi

echo
echo "== 旧版布局（参数写在单元里）仍要能读 =="

# 升级前装的那一版把地址直接写在 ExecStart 上。读不出来的话，装了旧版的板子
# 一跑体检就是一片默认值，比报错更误导人。
cat > "$TMP/legacy.service" <<'EOF'
[Service]
ExecStart=/opt/x30/bin/x30_gateway \
    --robot-ip 192.168.1.150 \
    --local-port 43897 \
    --perception-ip 192.168.1.160 \
    --serve --port 8888 --bind 192.168.10.5 \
    --web /opt/x30/web \
    --cloud --ros-host 192.168.1.120
EOF
OUT=$(X30_UNIT="$TMP/legacy.service" bash deploy/checkup.sh --config-only 2>&1)
for want in "运动主机 192.168.1.150" "感知主机 192.168.1.160" \
            "服务端口 8888" "监听 192.168.10.5" "点云 yes"; do
  if [[ "$OUT" == *"$want"* ]]; then
    pass "旧布局读到 $want"
  else
    fail "旧布局没读到 $want" "实得: $(echo "$OUT" | tr -s ' \n' ' ')"
  fi
done
if [[ "$OUT" == *"旧版布局"* ]]; then
  pass "提示了要重装一次才能在控制台里改"
else
  fail "没提示旧布局需要重装" "$OUT"
fi

echo
echo "== 多行 ExecStart 必须整段解析 =="
# 这一条专门盯着那个真实出过的问题：只 grep 第一行的话，
# 所有参数都抠不到，然后静静地退回默认值。
FIRST=$(grep '^ExecStart=' "$UNIT")
if [[ "$FIRST" == *"--config"* ]]; then
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
