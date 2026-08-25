#!/usr/bin/env bash
# 在 RK3588 上编译、安装并启用 X30 遥控网关。
#
#   sudo bash deploy/install.sh [--robot-ip 192.168.1.103] [--port 8080]
#
# 幂等：可反复执行以升级。重装时会先停服务，避免二进制被占用。

set -euo pipefail

SRC_EARLY="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=deploy/render_unit.sh
source "$SRC_EARLY/deploy/render_unit.sh"
# shellcheck source=deploy/config_util.sh
source "$SRC_EARLY/deploy/config_util.sh"
# shellcheck source=deploy/media_migrate.sh
source "$SRC_EARLY/deploy/media_migrate.sh"

PREFIX="/opt/x30"
UNIT_PATH="/etc/systemd/system/x30-gateway.service"

# 命令行给的值先存着，最后才盖上去。装完之后控制台可以在线改这些参数，
# 升级时不带参数重跑本脚本不该把人改过的值冲掉；但显式给了 --robot-ip
# 就必须以它为准并落进配置文件 —— 否则会出现"我明明装的时候指定了，怎么没生效"。
declare -A GIVEN=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --robot-ip)        GIVEN[robot_ip]="$2";        shift 2 ;;
    --robot-port)      GIVEN[robot_port]="$2";      shift 2 ;;
    --local-port)      GIVEN[local_port]="$2";      shift 2 ;;
    --perception-ip)   GIVEN[perception_ip]="$2";   shift 2 ;;
    --perception-port) GIVEN[perception_port]="$2"; shift 2 ;;
    --port)            GIVEN[http_port]="$2";       shift 2 ;;
    --bind)            GIVEN[bind_address]="$2";    shift 2 ;;
    --cloud)           GIVEN[cloud_enabled]="yes";  shift ;;
    --no-cloud)        GIVEN[cloud_enabled]="no";   shift ;;
    --ros-master)      GIVEN[ros_master]="$2";      shift 2 ;;
    --ros-host)        GIVEN[ros_host]="$2";        shift 2 ;;
    --cloud-topic)     GIVEN[cloud_topic]="$2";     shift 2 ;;
    --cloud-hz)        GIVEN[cloud_hz]="$2";        shift 2 ;;
    --cloud-points)    GIVEN[cloud_points]="$2";    shift 2 ;;
    --prefix)          PREFIX="$2"; shift 2 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

CONF="$PREFIX/conf/gateway.conf"
TOKEN_FILE="$PREFIX/conf/admin.token"

# --- 参数从哪来 -------------------------------------------------------------
# 优先级：命令行 > 已有配置文件 > 旧版单元里的内联参数 > 内置默认值。
#
# 中间那两级都是为了"升级时别把现场配好的东西冲掉"。尤其是最后那一级：
# 装过旧版的板子还没有配置文件，不从单元里把值搬过来的话，不带参数重跑本脚本
# 就会悄悄退回 192.168.1.103，而人根本不会想到去核对。
conf_defaults
if [[ -f $CONF ]]; then
  conf_load "$CONF"
  SEED=conf
elif conf_load_from_unit "$UNIT_PATH"; then
  SEED=unit
else
  SEED=default
fi

[[ -n ${GIVEN[robot_ip]+x} ]]        && ROBOT_IP=${GIVEN[robot_ip]}
[[ -n ${GIVEN[robot_port]+x} ]]      && ROBOT_PORT=${GIVEN[robot_port]}
[[ -n ${GIVEN[local_port]+x} ]]      && LOCAL_PORT=${GIVEN[local_port]}
[[ -n ${GIVEN[perception_ip]+x} ]]   && PERCEPTION_IP=${GIVEN[perception_ip]}
[[ -n ${GIVEN[perception_port]+x} ]] && PERCEPTION_PORT=${GIVEN[perception_port]}
[[ -n ${GIVEN[http_port]+x} ]]       && HTTP_PORT=${GIVEN[http_port]}
[[ -n ${GIVEN[bind_address]+x} ]]    && BIND_ADDR=${GIVEN[bind_address]}
[[ -n ${GIVEN[cloud_enabled]+x} ]]   && CLOUD=${GIVEN[cloud_enabled]}
[[ -n ${GIVEN[ros_master]+x} ]]      && ROS_MASTER=${GIVEN[ros_master]}
[[ -n ${GIVEN[ros_host]+x} ]]        && ROS_HOST=${GIVEN[ros_host]}
[[ -n ${GIVEN[cloud_topic]+x} ]]     && CLOUD_TOPIC=${GIVEN[cloud_topic]}
[[ -n ${GIVEN[cloud_hz]+x} ]]        && CLOUD_HZ=${GIVEN[cloud_hz]}
[[ -n ${GIVEN[cloud_points]+x} ]]    && CLOUD_POINTS=${GIVEN[cloud_points]}
true   # 上面最后一条 [[ ]] 为假时不该让 set -e 认为整个脚本失败

if [[ $EUID -ne 0 ]]; then
  echo "需要 root 权限，请用 sudo 运行。"
  exit 1
fi

SRC="$SRC_EARLY"
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
case "$SEED" in
  conf) echo "配置文件 : $CONF （已存在，未显式指定的参数沿用其中的值）" ;;
  unit) echo "配置文件 : $CONF （新建，参数从旧版 systemd 单元里搬过来）" ;;
  *)    echo "配置文件 : $CONF （新建）" ;;
esac
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
install -d -m 750 "$PREFIX/conf"
install -m 755 "$SRC/build/x30_gateway" "$PREFIX/bin/x30_gateway"
cp -r "$SRC/web/." "$PREFIX/web/"
cp "$SRC/docs/app-protocol.md" "$PREFIX/docs/" 2>/dev/null || true
cp "$SRC/docs/media-architecture.md" "$PREFIX/docs/" 2>/dev/null || true

# --- 运行配置与管理令牌 ------------------------------------------------------

# 配置文件是唯一的参数来源，systemd 单元只指向它。控制台的「设置」面板
# 改的也是这一份，所以这里的写入必须包含上面已经解析好的全部参数。
write_gateway_conf "$CONF"
chmod 644 "$CONF"
echo "已写入配置 $CONF"

# 令牌只生成一次。每次装机都换的话，现场记在本子上的那串就失效了，
# 而这时人多半已经不在板子旁边。
if [[ -s $TOKEN_FILE ]]; then
  echo "保留已有的管理令牌（未覆盖）"
else
  gen_admin_token > "$TOKEN_FILE"
  echo "已生成管理令牌"
fi
chmod 600 "$TOKEN_FILE"

# 网关进程要能读令牌、能改配置。它以 root 跑，这里只是把权限收到最小。
chown -R root:root "$PREFIX/conf"

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

# 老版本的 mediamtx.yml 把 WebRTC 绑在单一地址上，而 MESH 与 2.4G 落在两个互不
# 可达的网段（见 docs/media-architecture.md 第六节）—— 绑死一个，另一条链路必然
# 停在「等待拉流」。上面刻意不覆盖现场文件，所以这个修复只能靠迁移送进去：
# 只改 webrtc* 那几行，相机地址和密码一律不碰。
migrate_mediamtx_webrtc "$PREFIX/mediamtx.yml" || true

# --- systemd ----------------------------------------------------------------

render_unit "$SRC/deploy/x30-gateway.service" "$PREFIX" > "$UNIT_PATH"

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

地址、端口、点云这些参数以后可以直接在控制台的「设置」里改，不用再上命令行：
改完网关会自己重启，一两秒后恢复。设置密码是 54longqr。

配置在 ${CONF}。手工改配置文件也行，改完 systemctl restart x30-gateway。
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
