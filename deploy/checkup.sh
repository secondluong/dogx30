#!/usr/bin/env bash
# 装完之后跑一遍，一次性查清楚整条链路。
#
#   bash deploy/checkup.sh                # 全套
#   bash deploy/checkup.sh --config-only  # 只回显装的时候定了哪些参数
#
# 每一项要么过，要么给出**下一步该做什么**。装机当天人站在狗旁边，
# 不该还要翻六百行文档去对症状。
#
# 只读，不改任何配置，可以反复跑。不需要 root（个别项没权限会自己降级）。

set -u
cd "$(dirname "$0")/.." || exit 1

CONFIG_ONLY=no
[[ ${1:-} == --config-only ]] && CONFIG_ONLY=yes

OK=0; WARN=0; BAD=0

ok()   { printf '  [通过] %s\n' "$1"; OK=$((OK+1)); }
warn() { printf '  [注意] %s\n' "$1"; WARN=$((WARN+1))
         if [[ $# -gt 1 ]]; then printf '         %s\n' "$2"; fi; }
bad()  { printf '  [失败] %s\n' "$1"; BAD=$((BAD+1))
         if [[ $# -gt 1 ]]; then printf '         %s\n' "$2"; fi; }
note() { printf '         %s\n' "$1"; }
sect() { printf '\n== %s ==\n' "$1"; }

# 测试时可以指向一份夹具，正常使用不用管。
UNIT=${X30_UNIT:-/etc/systemd/system/x30-gateway.service}

sect "安装配置"
if [[ ! -f $UNIT ]]; then
  bad "没有找到 $UNIT" "还没装。先跑 sudo bash deploy/install.sh --robot-ip <运动主机IP>"
  echo
  echo "体检中止：服务都还没装，后面的项没有意义。"
  exit 1
fi

# --- 读出安装时定下的参数 ---------------------------------------------------
# 从 systemd 单元里反解，而不是让人再输一遍 —— 输错了这份体检就白做了。
#
# 少了这个文件就直接退出，不"尽力而为"地退回默认值：体检报着 192.168.1.103
# 而实际装的是别的地址，比体检跑不起来危险得多。
if [[ ! -f deploy/render_unit.sh ]]; then
  bad "找不到 deploy/render_unit.sh" "请在解压出来的完整源码目录里运行本脚本"
  exit 1
fi
# shellcheck source=deploy/render_unit.sh
source deploy/render_unit.sh

EXEC=$(unit_exec_args "$UNIT")
if [[ -z ${EXEC// /} ]]; then
  bad "$UNIT 里读不出 ExecStart" "单元文件可能被改坏了，重跑 install.sh"
  exit 1
fi

arg_of() {                      # arg_of --robot-ip  ->  192.168.1.103
  echo "$EXEC" | tr ' ' '\n' | grep -A1 -x -- "$1" | sed -n 2p
}

ROBOT_IP=$(arg_of --robot-ip);      ROBOT_IP=${ROBOT_IP:-192.168.1.103}
PERC_IP=$(arg_of --perception-ip);  PERC_IP=${PERC_IP:-192.168.1.105}
HTTP_PORT=$(arg_of --port);         HTTP_PORT=${HTTP_PORT:-8080}
BIND_ADDR=$(arg_of --bind);         BIND_ADDR=${BIND_ADDR:-0.0.0.0}
ROS_HOST=$(arg_of --ros-host);      ROS_HOST=${ROS_HOST:-}

CLOUD=no
case " $EXEC " in *" --cloud "*) CLOUD=yes ;; esac

# 绑到具体网卡时不能用回环去连自己
PROBE_HOST=$BIND_ADDR
if [[ $BIND_ADDR == "0.0.0.0" ]]; then PROBE_HOST=127.0.0.1; fi

echo "  运动主机 $ROBOT_IP   感知主机 $PERC_IP"
echo "  服务端口 $HTTP_PORT   监听 $BIND_ADDR   点云 $CLOUD"

[[ $CONFIG_ONLY == yes ]] && exit 0

# --- 服务 -------------------------------------------------------------------

sect "服务"

if systemctl is-active --quiet x30-gateway; then
  ok "x30-gateway 正在运行"
else
  bad "x30-gateway 没有运行" "journalctl -u x30-gateway -n 40 --no-pager 看原因"
fi

if systemctl is-enabled --quiet x30-gateway 2>/dev/null; then
  ok "x30-gateway 已设为开机自启"
else
  warn "x30-gateway 没有开机自启" "systemctl enable x30-gateway"
fi

if [[ -f /etc/systemd/system/x30-media.service ]]; then
  if systemctl is-active --quiet x30-media; then
    ok "x30-media 正在运行"
  else
    warn "x30-media 没有运行（控制不受影响，只是没视频）" \
         "journalctl -u x30-media -n 30 --no-pager"
  fi
else
  warn "没装媒体服务，视频不可用" \
       "把 mediamtx 的 linux-arm64 二进制放到 /opt/x30/bin/mediamtx 后重跑 install.sh"
fi

# --- 监听与暴露面 -----------------------------------------------------------

sect "监听与暴露面"

LISTEN=$(ss -ltn 2>/dev/null | awk -v p=":$HTTP_PORT" '$4 ~ p {print $4}' | sed -n 1p)
if [[ -n $LISTEN ]]; then
  ok "服务端口在监听（$LISTEN）"
else
  bad "端口 $HTTP_PORT 没有在监听" "服务可能起来了又退了，看 journalctl"
fi

# 有 4G 的话，绑 0.0.0.0 等于把一个无鉴权的控制口挂到广域网上
WAN=$(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' |
      grep -E '^(ppp|wwan|usb)' | tr '\n' ' ')
if [[ $BIND_ADDR == "0.0.0.0" ]]; then
  if [[ -n $WAN ]]; then
    bad "监听全部网卡，而本机有广域接口（$WAN）" \
        "协议无身份认证。请用 --bind <遥控链路地址> 重装，例如 --bind 192.168.10.2"
  else
    warn "监听全部网卡（当前没发现 4G/WWAN 接口）" \
         "以后插上 4G 模块的话记得改成 --bind"
  fi
else
  ok "监听地址已限定在 $BIND_ADDR"
fi

# --- 机器狗 -----------------------------------------------------------------

sect "机器狗"

if ping -c1 -W2 "$ROBOT_IP" >/dev/null 2>&1; then
  ok "运动主机 $ROBOT_IP 可达"
else
  bad "运动主机 $ROBOT_IP ping 不通" \
      "查网线，以及本机 eth0 的静态地址（应在 192.168.1.x/24 且不与狗冲突）"
fi

if ping -c1 -W2 "$PERC_IP" >/dev/null 2>&1; then
  ok "感知主机 $PERC_IP 可达"
else
  warn "感知主机 $PERC_IP ping 不通" \
       "上下楼靠它的地形图模块，点云和机身相机也在它上面，不通则这几项都用不了"
fi

# .106 是智能控制器。早期资料说本机没有，但厂家 2026-08 的拓扑图上画着，
# 两种说法对不上。通与不通都要往回报：通了说明官方 jy_cog 建图定位可用，
# 自建 SLAM 那一大块可能整个省掉，对排期影响很大。
NAV_IP=192.168.1.106
if ping -c1 -W2 "$NAV_IP" >/dev/null 2>&1; then
  ok "智能控制器 $NAV_IP 在（与厂家拓扑图一致）"
  note "官方 jy_cog 建图定位可能可用，自建 SLAM 也许能省掉。请反馈这条结果"
  note "登进去看看有没有 ~/jy_cog/transfer/setup.bash，有的话 CustomMsg 定义直接可用"
else
  note "智能控制器 $NAV_IP 不在 —— 与早期资料一致，与厂家拓扑图不符"
  note "SLAM 要自己在板子上做，点云目前只显示当前帧、不累积成图。请反馈这条结果"
fi

# 板子必须和运动主机同处 192.168.1.0/24。尾部调试网口和交换机之间隔着一个
# 路由器（见厂家拓扑图），万一它在做 NAT，板子就落在别的网段上 ——
# 那样指令发得出去、遥测永远回不来，症状和 network.toml 没登记一模一样。
# 单靠 ping 区分不出这两种情况，所以这里直接看本机地址。
DOG_NET=${ROBOT_IP%.*}.
if ip -4 addr show 2>/dev/null | grep -q "inet $DOG_NET"; then
  ok "本机在机器狗网段 ${DOG_NET}0/24 内"
else
  bad "本机没有 ${DOG_NET}0/24 网段的地址" \
      "运动主机的遥测是单播，收不到。多半是尾部调试网口那个路由器在做 NAT，
         换成接交换机空口，或把路由器改桥接。当前本机地址：
         $(ip -4 -br addr show 2>/dev/null | tr '\n' ' ')"
fi

# 最关键的一项：能 ping 通不代表收得到遥测。运动主机只往 network.toml 里
# 登记过的地址回发，没登记的症状就是"指令发得出去、数据收不回来"。
sect "遥测（能 ping 通 ≠ 收得到数据）"

if ! command -v python3 >/dev/null 2>&1; then
  warn "没有 python3，跳过遥测检查" "手动看控制台的遥测盘是不是灰的"
elif [[ ! -f tools/ws_probe.py ]]; then
  warn "找不到 tools/ws_probe.py，跳过遥测检查" "请在解压出来的源码目录里运行本脚本"
else
  TELEM=$(python3 tools/state_once.py "$PROBE_HOST" "$HTTP_PORT" 2>&1)
  case "$TELEM" in
    ALIVE*)
      ok "网关正在收到运动主机的遥测   ${TELEM#ALIVE }"
      ;;
    STALE*)
      bad "网关连得上，但收不到运动主机的遥测" \
          "多半是 network.toml 没登记本机。登到 $ROBOT_IP 上改
         /home/ysc/jy_exe/conf/network.toml，把本机 IP 与端口加进 ips/ports
         （两个数组要索引对齐），然后重启运动程序"
      ;;
    *)
      bad "连不上本机的遥控服务 $PROBE_HOST:$HTTP_PORT" "$TELEM"
      ;;
  esac
fi

# --- 点云 -------------------------------------------------------------------

sect "点云"

if [[ $CLOUD != yes ]]; then
  note "未开启（默认如此）。确认下面 ROS master 可达后，用 install.sh 加"
  note "--cloud --ros-host <与狗直连那块网卡的地址> 重装即可打开。"
fi

if curl -s --max-time 3 "http://$PERC_IP:11311/" >/dev/null 2>&1; then
  ok "ROS master http://$PERC_IP:11311 有响应"
elif [[ $CLOUD == yes ]]; then
  bad "点云已开启，但 ROS master 连不上" "感知主机上的 roscore 没起，或地址不对"
else
  warn "ROS master 无响应，点云暂时开不了" "先在感知主机上确认 roscore 在跑"
fi

if [[ $CLOUD == yes && -n $ROS_HOST ]]; then
  if ip -4 addr show 2>/dev/null | grep -q "inet $ROS_HOST/"; then
    ok "--ros-host $ROS_HOST 确实是本机地址"
  else
    bad "--ros-host $ROS_HOST 不是本机任何一块网卡的地址" \
        "感知主机要靠这个地址反连回来送数据。填错的症状是订阅成功但一帧都收不到"
  fi
fi

# --- 相机 -------------------------------------------------------------------

sect "相机"

DOG_CAM="rtsp://$PERC_IP:8554/test"
if command -v python3 >/dev/null 2>&1 && [[ -f tools/rtsp_probe.py ]]; then
  if python3 tools/rtsp_probe.py "$DOG_CAM" --timeout 4 >/tmp/x30cam.txt 2>&1; then
    ok "机身相机可取流   $(grep -E '视频编码|分辨率|音频' /tmp/x30cam.txt |
                          tr -d ' ' | tr '\n' ' ')"
    note "完整输出在 /tmp/x30cam.txt，据此核对 /opt/x30/media.json 的 codec 与 kbps"
  else
    warn "机身相机取不到流（$DOG_CAM）" \
         "这个接口不在官方文档里，换固件后路径可能变。详见 /tmp/x30cam.txt"
  fi
else
  note "跳过（缺 python3 或 tools/rtsp_probe.py）"
fi

if [[ -f /etc/systemd/system/x30-media.service ]]; then
  PATHS=$(curl -s --max-time 3 http://127.0.0.1:9997/v3/paths/list 2>/dev/null)
  if [[ -n $PATHS ]]; then
    RDY=$(echo "$PATHS" | grep -c '"ready":true')
    ok "MediaMTX 管理接口有响应，已就绪的路径 $RDY 条"
    note "按需拉流的路径在没人看时就是没就绪，这不算问题"
  else
    warn "MediaMTX 管理接口没响应" "journalctl -u x30-media -n 30 --no-pager"
  fi
fi

# --- 本机 -------------------------------------------------------------------

sect "本机"

GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)
if [[ $GOV == performance ]]; then
  ok "CPU 调度器为 performance"
elif [[ -n $GOV ]]; then
  warn "CPU 调度器是 $GOV" "长时间跑视频和点云建议设成 performance，否则会有卡顿"
fi

TZONE=$(find /sys/class/thermal -name temp 2>/dev/null | sed -n 1p)
if [[ -n $TZONE ]]; then
  T=$(awk '{printf "%.1f", $1/1000}' "$TZONE" 2>/dev/null || true)
  if [[ -n $T ]]; then
    if awk "BEGIN{exit !($T > 80)}"; then
      bad "SoC 温度 ${T}°C，已进入降频区" "加散热片或风扇；装在密闭外壳里尤其要留意"
    elif awk "BEGIN{exit !($T > 70)}"; then
      warn "SoC 温度 ${T}°C 偏高" "持续满负载会降频"
    else
      ok "SoC 温度 ${T}°C"
    fi
  fi
fi

FREE=$(df -m / 2>/dev/null | awk 'NR==2{print $4}')
if [[ -n $FREE ]]; then
  if [[ $FREE -lt 500 ]]; then
    warn "根分区只剩 ${FREE}MB" "日志会持续增长，清一下或限制 journald 大小"
  else
    ok "根分区剩余 ${FREE}MB"
  fi
fi

# --- 小结 -------------------------------------------------------------------

printf '\n小结：通过 %d，注意 %d，失败 %d\n' "$OK" "$WARN" "$BAD"

if [[ $BAD -gt 0 ]]; then
  echo "有硬伤，先把上面标[失败]的处理掉。"
  exit 1
fi
if [[ $WARN -gt 0 ]]; then
  echo "可以用了，但标[注意]的项会限制部分功能。"
  exit 0
fi
echo "整条链路都通了。"
