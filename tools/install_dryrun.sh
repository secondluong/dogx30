#!/usr/bin/env bash
# 空跑 install.sh 的两件产出：systemd 单元与 gateway.conf。
#
# 装一次要上板子、要 root、要停服务，反馈很慢。而这两样拼错的后果是服务起不来
# 或者参数悄悄没生效 —— 后者尤其恶心：看着一切正常，就是没数据。
# 所以单独拎出来在本地验。
#
# 关键是最后那一步：让网关自己去解析。手写断言只能证明字符串对，
# 证明不了网关认得这些东西 —— 键名拼错、值超范围，它会直接拒绝启动。

set -u
cd "$(dirname "$0")/.." || exit 1

FAILED=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

GATEWAY_BIN="./build/x30_gateway"
[[ -x "$GATEWAY_BIN" ]] || GATEWAY_BIN="./build-wsl/x30_gateway"

# 用 install.sh 真正在用的那两个函数，而不是照着抄一份
# shellcheck source=deploy/render_unit.sh
source deploy/render_unit.sh
# shellcheck source=deploy/config_util.sh
source deploy/config_util.sh

pass() { echo "   [OK]   $1"; }
fail() { echo "   [FAIL] $1${2:+  $2}"; FAILED=1; }

# ---------------------------------------------------------------------------
echo "== systemd 单元 =="
# ---------------------------------------------------------------------------

# 单元里只剩安装前缀要替换。运行参数搬进配置文件了 —— 两处各存一份的话，
# 迟早出现「单元里写着 .103、实际跑的是 .200」。
UNIT="$TMP/x30.service"
render_unit deploy/x30-gateway.service "$TMP/opt" > "$UNIT"
ARGS=$(unit_exec_args "$UNIT" | sed -e 's|[^ ]*/x30_gateway||')
echo "   $ARGS"

for expect in "--config $TMP/opt/conf/gateway.conf" \
              "--admin-token-file $TMP/opt/conf/admin.token" \
              "--serve" "--web $TMP/opt/web" "--media $TMP/opt/media.json"; do
  if [[ "$ARGS" == *"$expect"* ]]; then
    pass "含 $expect"
  else
    fail "缺 $expect"
  fi
done

# 前缀必须换干净。漏一处的话，服务会去读 /opt/x30 下不存在的东西。
if [[ "$ARGS" == *"/opt/x30"* ]]; then
  fail "还残留着 /opt/x30" "--prefix 换了个位置就会读错文件"
else
  pass "安装前缀已全部替换"
fi

# 地址端口不该再出现在单元里，否则就又有两份配置了。
for leak in "--robot-ip" "--perception-ip" "--bind" "--port " "--cloud"; do
  if [[ "$ARGS" == *"$leak"* ]]; then
    fail "单元里出现了 $leak" "这些参数应当只存在于 gateway.conf"
  fi
done
pass "单元里没有重复配置地址与端口"

# conf/ 必须可写，否则控制台存不了配置（ProtectSystem=strict 把别处都锁了）。
if grep -q '^ReadWritePaths=.*/conf' "$UNIT"; then
  pass "conf/ 在 ReadWritePaths 里"
else
  fail "conf/ 不在 ReadWritePaths 里" "控制台会写不进配置文件"
fi

# 改配置走的是正常退出，靠 Restart=always 把网关拉回来。
if grep -q '^Restart=always' "$UNIT"; then
  pass "Restart=always（改完配置能自己回来）"
else
  fail "Restart 不是 always" "改配置后网关退出就再也起不来了"
fi

# ---------------------------------------------------------------------------
echo
echo "== gateway.conf =="
# ---------------------------------------------------------------------------

# check_conf <标题> <期望片段…>，其余参数从当前的全局变量取
check_conf() {
  local title=$1; shift
  local conf="$TMP/gateway.conf"
  write_gateway_conf "$conf"

  echo
  echo "-- $title"
  for expect in "$@"; do
    if grep -qx -- "$expect" "$conf"; then
      pass "含 $expect"
    else
      fail "缺 $expect" "实得: $(grep -v '^#' "$conf" | tr -s '\n' ' ')"
    fi
  done

  # 让网关自己读一遍。它会逐项校验，读不懂就非零退出并说明哪一行。
  if [[ -x "$GATEWAY_BIN" ]]; then
    if timeout 5 "$GATEWAY_BIN" --config "$conf" --help > /dev/null 2>"$TMP/err"; then
      pass "网关接受这份配置"
    else
      fail "网关拒绝这份配置" "$(head -2 "$TMP/err")"
    fi
  fi

  # 体检脚本必须能把同一份文件原样读回来。写得出来读不回来是最难查的一类错。
  local got
  got=$(conf_get "$conf" robot_ip)
  if [[ $got == "$ROBOT_IP" ]]; then
    pass "conf_get 读回 robot_ip = $got"
  else
    fail "conf_get 读回的 robot_ip 是 $got" "应为 $ROBOT_IP"
  fi
}

conf_defaults
check_conf "默认（点云关闭）" \
  "robot_ip = 192.168.1.103" \
  "perception_ip = 192.168.1.105" \
  "http_port = 8080" \
  "bind_address = 0.0.0.0" \
  "cloud_enabled = no" \
  "ros_master = http://192.168.1.105:11311"

conf_defaults
BIND_ADDR="192.168.10.2"
CLOUD="yes"
ROS_HOST="192.168.1.120"
check_conf "收紧监听 + 开点云" \
  "bind_address = 192.168.10.2" \
  "cloud_enabled = yes" \
  "ros_host = 192.168.1.120"

conf_defaults
PERCEPTION_IP="192.168.1.200"
CLOUD="yes"
check_conf "非默认感知主机（ROS master 要跟着走）" \
  "perception_ip = 192.168.1.200" \
  "ros_master = http://192.168.1.200:11311"

conf_defaults
ROBOT_IP="192.168.1.106"
ROBOT_PORT="43894"
LOCAL_PORT="43898"
PERCEPTION_PORT="43900"
HTTP_PORT="9090"
CLOUD_TOPIC="/points_raw"
CLOUD_HZ="5"
CLOUD_POINTS="30000"
check_conf "全部参数都非默认" \
  "robot_ip = 192.168.1.106" \
  "robot_port = 43894" \
  "local_port = 43898" \
  "perception_port = 43900" \
  "http_port = 9090" \
  "cloud_topic = /points_raw" \
  "cloud_hz = 5" \
  "cloud_points = 30000"

# 点云关闭时绝不能写成开启，否则开机就会去连一台没验证过的生产设备。
echo
echo "-- 点云关闭时必须是 cloud_enabled = no"
conf_defaults
write_gateway_conf "$TMP/off.conf"
if grep -qx "cloud_enabled = no" "$TMP/off.conf"; then
  pass "cloud_enabled = no"
else
  fail "点云默认没关" "$(grep cloud_enabled "$TMP/off.conf")"
fi

# ---------------------------------------------------------------------------
echo
echo "== 从旧版单元迁移 =="
# ---------------------------------------------------------------------------

# 装过旧版的板子还没有配置文件。升级时若不把单元里的内联参数搬过来，
# 不带参数重跑 install.sh 就会悄悄退回 192.168.1.103 —— 人明明装的时候
# 指定过别的地址，而且不会想到去核对。这是升级路径上最容易伤到现场的一处。
cat > "$TMP/legacy.service" <<'EOF'
[Service]
ExecStart=/opt/x30/bin/x30_gateway \
    --robot-ip 192.168.1.200 \
    --local-port 43899 \
    --perception-ip 192.168.1.210 \
    --serve --port 9099 --bind 192.168.10.7 \
    --web /opt/x30/web \
    --cloud --ros-master http://192.168.1.210:11311 --ros-host 192.168.1.121
EOF

conf_defaults
if conf_load_from_unit "$TMP/legacy.service"; then
  pass "认出这是旧版布局"
else
  fail "没认出旧版布局" "升级会把现场配置冲回默认值"
fi
for pair in "ROBOT_IP=192.168.1.200" "LOCAL_PORT=43899" \
            "PERCEPTION_IP=192.168.1.210" "HTTP_PORT=9099" \
            "BIND_ADDR=192.168.10.7" "CLOUD=yes" \
            "ROS_HOST=192.168.1.121" "ROS_MASTER=http://192.168.1.210:11311"; do
  name=${pair%%=*}; want=${pair#*=}
  if [[ ${!name} == "$want" ]]; then
    pass "搬过来了 $name = $want"
  else
    fail "$name 搬成了 ${!name}" "应为 $want"
  fi
done

# -x 那一条不能少：不加的话 --port 会先匹配到 --perception-port / --cloud-points。
if [[ $HTTP_PORT == "9099" && $PERCEPTION_PORT == "43899" ]]; then
  pass "--port 没有误匹配到 --perception-port"
else
  fail "--port 匹配串味了" "http_port=$HTTP_PORT perception_port=$PERCEPTION_PORT"
fi

# 新布局的单元里没有内联参数，必须被认出来不是旧版，否则会拿一堆空值去覆盖。
conf_defaults
if conf_load_from_unit "$UNIT"; then
  fail "把新布局的单元当成了旧版" "会拿空值覆盖掉真实配置"
else
  pass "新布局的单元不会被当成旧版"
fi
if [[ $ROBOT_IP == "192.168.1.103" ]]; then
  pass "误判被拒后变量没被动过"
else
  fail "变量被污染了" "robot_ip=$ROBOT_IP"
fi

# ---------------------------------------------------------------------------
echo
echo "== 坏配置必须被拒绝，不能退回默认值 =="
# ---------------------------------------------------------------------------

# 悄悄退回默认值意味着网关连到 192.168.1.103，而文件里明明写着别的地址。
# 这种故障现场没人查得出来，所以宁可起不来。
if [[ -x "$GATEWAY_BIN" ]]; then
  reject() {   # reject <标题> <文件内容>
    local title=$1 body=$2
    printf '%s\n' "$body" > "$TMP/bad.conf"
    if timeout 5 "$GATEWAY_BIN" --config "$TMP/bad.conf" --help \
         > /dev/null 2>"$TMP/e"; then
      fail "$title 被接受了" "应当拒绝启动"
    else
      pass "$title 被拒绝（$(head -1 "$TMP/e" | cut -c1-72)）"
    fi
  }
  reject "键名拼错"     "robot_ipp = 192.168.1.9"
  reject "端口超范围"   "http_port = 99999"
  reject "端口不是数字" "http_port = 八千零八十"
  reject "点云开关写错" "cloud_enabled = maybe"
  reject "帧率超范围"   "cloud_hz = 999"
  reject "不是键值对"   "robot_ip 192.168.1.9"
else
  echo "   （没有编译产物，跳过网关自解析部分）"
fi

echo
if [[ $FAILED -eq 0 ]]; then
  echo "install.sh 产出检查通过"
else
  echo "install.sh 产出检查失败"
fi
exit $FAILED
