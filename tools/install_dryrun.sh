#!/usr/bin/env bash
# 空跑 install.sh 的 systemd 单元生成，检查参数替换有没有拼错。
#
# 装一次要上板子、要 root、要停服务，反馈很慢。而这段 sed 拼错的后果是
# 服务起不来或者参数悄悄没生效 —— 后者尤其恶心：看着一切正常，就是没数据。
# 所以单独拎出来在本地验。

set -u
cd "$(dirname "$0")/.." || exit 1

FAILED=0
GATEWAY_BIN="./build/x30_gateway"
[[ -x "$GATEWAY_BIN" ]] || GATEWAY_BIN="./build-wsl/x30_gateway"

# 用 install.sh 真正在用的那个函数，而不是照着抄一份
# shellcheck source=deploy/render_unit.sh
source deploy/render_unit.sh

render() {   # render <bind> <perception> <cloud> <ros_host>
  render_unit deploy/x30-gateway.service \
    192.168.1.103 "$2" 43897 8080 "$1" "$3" "$4" /opt/x30
}

# 去掉程序名，只留参数
extract_args() {
  local f=$1
  unit_exec_args "$f" | sed -e 's|[^ ]*/x30_gateway||'
}

check_case() {
  local title=$1 bind=$2 perception=$3 cloud=$4 ros_host=$5
  shift 5

  local args unit
  unit=$(mktemp)
  render "$bind" "$perception" "$cloud" "$ros_host" > "$unit"
  args=$(extract_args "$unit")
  rm -f "$unit"

  echo
  echo "-- $title"
  echo "   $args"

  for expect in "$@"; do
    if [[ "$args" == *"$expect"* ]]; then
      echo "   [OK]   含 $expect"
    else
      echo "   [FAIL] 缺 $expect"
      FAILED=1
    fi
  done

  # 真正拿网关自己解析一遍。手写断言只能证明字符串对，
  # 证明不了这些参数网关认得 —— 拼错一个选项名它会直接退出。
  if [[ -x "$GATEWAY_BIN" ]]; then
    # shellcheck disable=SC2086
    if timeout 5 $GATEWAY_BIN $args --help > /dev/null 2>&1; then
      echo "   [OK]   网关接受这组参数"
    else
      echo "   [FAIL] 网关拒绝这组参数"
      FAILED=1
    fi
  fi
}

check_case "默认（点云关闭）" "0.0.0.0" "192.168.1.105" "no" "192.168.1.120" \
  "--bind 0.0.0.0" "--perception-ip 192.168.1.105" "--media"

check_case "收紧监听 + 开点云" "192.168.10.2" "192.168.1.105" "yes" "192.168.1.120" \
  "--bind 192.168.10.2" "--cloud" "--ros-host 192.168.1.120" \
  "--ros-master http://192.168.1.105:11311"

check_case "非默认感知主机" "192.168.10.2" "192.168.1.200" "yes" "192.168.1.120" \
  "--perception-ip 192.168.1.200" \
  "--ros-master http://192.168.1.200:11311"

# 点云关闭时绝不能出现 --cloud，否则开机就会去连一台没验证过的生产设备。
echo
echo "-- 点云关闭时不得出现 --cloud"
if render "0.0.0.0" "192.168.1.105" "no" "192.168.1.120" | grep -q -- "--cloud"; then
  echo "   [FAIL] 出现了 --cloud"
  FAILED=1
else
  echo "   [OK]   没有 --cloud"
fi

echo
if [[ $FAILED -eq 0 ]]; then
  echo "install.sh 参数替换检查通过"
else
  echo "install.sh 参数替换检查失败"
fi
exit $FAILED
