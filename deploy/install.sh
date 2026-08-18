#!/usr/bin/env bash
# 在 RK3588 上编译、安装并启用 X30 遥控网关。
#
#   sudo bash deploy/install.sh [--robot-ip 192.168.1.103] [--port 8080]
#
# 幂等：可反复执行以升级。重装时会先停服务，避免二进制被占用。

set -euo pipefail

ROBOT_IP="192.168.1.103"
PERCEPTION_IP="192.168.1.105"
LOCAL_PORT="43897"
HTTP_PORT="8080"
BIND_ADDR="0.0.0.0"
PREFIX="/opt/x30"
# 点云默认关闭：感知主机的 ROS 可达性没有现场验证过之前，
# 不该在开机时就去连一台生产设备。验通了再用 --cloud 打开。
CLOUD="no"
ROS_HOST="192.168.1.120"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --robot-ip)      ROBOT_IP="$2"; shift 2 ;;
    --perception-ip) PERCEPTION_IP="$2"; shift 2 ;;
    --local-port)    LOCAL_PORT="$2"; shift 2 ;;
    --port)          HTTP_PORT="$2"; shift 2 ;;
    --bind)          BIND_ADDR="$2"; shift 2 ;;
    --prefix)        PREFIX="$2"; shift 2 ;;
    --cloud)         CLOUD="yes"; shift ;;
    --ros-host)      ROS_HOST="$2"; shift 2 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "需要 root 权限，请用 sudo 运行。"
  exit 1
fi

SRC="$(cd "$(dirname "$0")/.." && pwd)"
echo "源码目录 : $SRC"
echo "安装位置 : $PREFIX"
echo "运动主机 : $ROBOT_IP  本机遥测端口 $LOCAL_PORT"
echo "感知主机 : $PERCEPTION_IP  （地形图，上下楼必需）"
echo "服务端口 : $HTTP_PORT  监听地址 $BIND_ADDR"
if [[ "$CLOUD" == "yes" ]]; then
  echo "点云     : 开启，本机在 ROS 网络中的地址 $ROS_HOST"
else
  echo "点云     : 关闭（确认感知主机 ROS 可达后，加 --cloud 重装）"
fi
echo

# 感知主机要反向连回来取点云数据，所以这个地址必须是与狗直连的那块网卡。
# 填成 MESH 侧地址的话注册会成功但收不到任何数据，现场极难查，所以提前拦一道。
if [[ "$CLOUD" == "yes" && "$ROS_HOST" == "$BIND_ADDR" && "$BIND_ADDR" != "0.0.0.0" ]]; then
  echo "警告: --ros-host 与 --bind 相同（$ROS_HOST）。"
  echo "      前者应是与机器狗直连的网卡地址，后者是遥控链路（MESH）的地址，"
  echo "      两者通常不同。填错的症状是订阅成功但一条点云都收不到。"
  echo
fi

if [[ "$BIND_ADDR" == "0.0.0.0" ]]; then
  echo "注意: 将监听全部网卡。协议无身份认证，凡能连上此端口者皆可申请控制权。"
  echo "      本机若接有 4G 等广域接口，请改用 --bind <遥控链路地址> 重新安装。"
  echo
fi

# --- 依赖 -------------------------------------------------------------------

for tool in cmake g++ make; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "缺少 $tool。请先执行： apt-get install -y build-essential cmake"
    exit 1
  fi
done

# --- 编译 -------------------------------------------------------------------

echo "编译中…"
cmake -S "$SRC/rk3588" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$SRC/build" -j"$(nproc)"

# --- 安装 -------------------------------------------------------------------

# 先停服务，否则正在运行的二进制无法覆盖。
if systemctl is-active --quiet x30-gateway; then
  echo "停止运行中的服务…"
  systemctl stop x30-gateway
fi

install -d "$PREFIX/bin" "$PREFIX/web" "$PREFIX/docs" "$PREFIX/log"
install -m 755 "$SRC/build/x30_gateway" "$PREFIX/bin/x30_gateway"
cp -r "$SRC/web/." "$PREFIX/web/"
cp "$SRC/docs/app-protocol.md" "$PREFIX/docs/" 2>/dev/null || true
cp "$SRC/docs/media-architecture.md" "$PREFIX/docs/" 2>/dev/null || true

# 媒体配置里有相机地址和密码，是现场一台一改的东西，**不能覆盖已有的**，
# 否则每次升级都要重填一遍，迟早出事。
for f in media.json mediamtx.yml; do
  if [[ -f "$PREFIX/$f" ]]; then
    echo "保留已有的 $f（未覆盖）"
  else
    cp "$SRC/deploy/$f" "$PREFIX/$f"
    echo "已放置 $f 样例，需按现场相机地址修改"
  fi
done

# --- systemd ----------------------------------------------------------------

# shellcheck source=deploy/render_unit.sh
source "$SRC/deploy/render_unit.sh"

render_unit "$SRC/deploy/x30-gateway.service" \
    "$ROBOT_IP" "$PERCEPTION_IP" "$LOCAL_PORT" \
    "$HTTP_PORT" "$BIND_ADDR" "$CLOUD" "$ROS_HOST" "$PREFIX" \
    > /etc/systemd/system/x30-gateway.service

# 媒体服务是独立单元，只在 mediamtx 二进制存在时才装。它是可选的：
# 没有它控制照样跑，只是没有视频。
if [[ -x "$PREFIX/bin/mediamtx" ]]; then
  sed -e "s|/opt/x30|$PREFIX|g" \
      "$SRC/deploy/x30-media.service" > /etc/systemd/system/x30-media.service
  MEDIA=1
else
  MEDIA=0
fi

systemctl daemon-reload
systemctl enable x30-gateway >/dev/null
systemctl restart x30-gateway

if [[ "$MEDIA" == "1" ]]; then
  systemctl enable x30-media >/dev/null
  systemctl restart x30-media
fi

sleep 2
if ! systemctl is-active --quiet x30-gateway; then
  echo
  echo "服务启动失败，最近日志："
  journalctl -u x30-gateway -n 30 --no-pager
  exit 1
fi

if [[ "$MEDIA" == "1" ]] && ! systemctl is-active --quiet x30-media; then
  echo
  echo "注意: 媒体服务未能启动，控制功能不受影响。日志："
  journalctl -u x30-media -n 20 --no-pager
fi

IP=$(hostname -I | awk '{print $1}')
cat <<EOF

安装完成。

  控制台   http://${IP}:${HTTP_PORT}/
  查看日志 journalctl -u x30-gateway -f
  重启     systemctl restart x30-gateway
  停止     systemctl stop x30-gateway

遥控端连同一个网段后，浏览器打开上面的地址即可。
EOF

if [[ "$MEDIA" == "1" ]]; then
  cat <<EOF
媒体服务已启用：
  日志     journalctl -u x30-media -f
  配置     $PREFIX/mediamtx.yml 与 $PREFIX/media.json（两处的 path 要对应）
EOF
else
  cat <<EOF
未安装媒体服务（视频不可用）。要启用的话，把 mediamtx 的 linux-arm64 二进制
放到 $PREFIX/bin/mediamtx 后重跑本脚本：
  https://github.com/bluenviron/mediamtx/releases
EOF
fi
