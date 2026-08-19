#!/usr/bin/env bash
# 向导式安装：把 bootstrap → 编译 → install → checkup 串成一条，中途问几个参数。
#
#   sudo bash deploy/install_gui.sh                    # 交互，一路回车即可
#   sudo bash deploy/install_gui.sh --yes              # 全用默认值，不问
#   sudo bash deploy/install_gui.sh --yes --cloud \
#        --robot-ip 192.168.1.103 --bind 192.168.10.2  # 脚本化调用
#
# 存在的意义不是少敲几条命令，而是**少漏一步**。装机当天人蹲在狗旁边，
# 顺序记错、参数填错都很常见，而其中几种错法的现场症状一模一样。
#
# 幂等，可反复执行。已经做过的步骤会自动跳过。

set -u

cd "$(dirname "$0")/.." || exit 1
SRC=$(pwd)

# --- 外观 -------------------------------------------------------------------

if [[ -t 1 ]]; then
  B=$'\033[1m'; DIM=$'\033[2m'; R=$'\033[31m'; G=$'\033[32m'
  Y=$'\033[33m'; N=$'\033[0m'
else
  B=""; DIM=""; R=""; G=""; Y=""; N=""
fi

TOTAL=6
step() { printf '\n%s[%d/%d] %s%s\n' "$B" "$1" "$TOTAL" "$2" "$N"; }
ok()   { printf '  %s✓%s %s\n' "$G" "$N" "$1"; }
warn() { printf '  %s!%s %s\n' "$Y" "$N" "$1"; }
die()  { printf '\n  %s✗ %s%s\n\n' "$R" "$1" "$N"; exit 1; }
info() { printf '    %s%s%s\n' "$DIM" "$1" "$N"; }

# --- 参数 -------------------------------------------------------------------

ROBOT_IP="192.168.1.103"
PERCEPTION_IP="192.168.1.105"
HTTP_PORT="8080"
BIND_ADDR=""            # 空 = 稍后询问或自动判断
CLOUD="no"
ROS_HOST=""
ASSUME_YES=0
SKIP_BOOTSTRAP=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --robot-ip)       ROBOT_IP="$2"; shift 2 ;;
    --perception-ip)  PERCEPTION_IP="$2"; shift 2 ;;
    --port)           HTTP_PORT="$2"; shift 2 ;;
    --bind)           BIND_ADDR="$2"; shift 2 ;;
    --cloud)          CLOUD="yes"; shift ;;
    --ros-host)       ROS_HOST="$2"; shift 2 ;;
    -y|--yes)         ASSUME_YES=1; shift ;;
    --skip-bootstrap) SKIP_BOOTSTRAP=1; shift ;;
    -h|--help)
      sed -n '2,15p' "$0" | sed 's/^# \?//'
      exit 0 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

# --- 提问工具 ---------------------------------------------------------------

# ask <提示> <默认值> -> 结果写进 REPLY_VAL
ask() {
  local prompt=$1 default=$2
  if [[ $ASSUME_YES -eq 1 ]] || [[ ! -t 0 ]]; then
    REPLY_VAL="$default"
    printf '  %s：%s %s(默认)%s\n' "$prompt" "$default" "$DIM" "$N"
    return
  fi
  local input
  read -r -p "  $prompt [$default]: " input
  REPLY_VAL="${input:-$default}"
}

# ask_yn <提示> <默认 y|n>
ask_yn() {
  local prompt=$1 default=$2
  if [[ $ASSUME_YES -eq 1 ]] || [[ ! -t 0 ]]; then
    [[ $default == y ]]; return
  fi
  local hint="[y/N]"; [[ $default == y ]] && hint="[Y/n]"
  local input
  read -r -p "  $prompt $hint: " input
  input="${input:-$default}"
  [[ ${input,,} == y* ]]
}

# --- root -------------------------------------------------------------------

if [[ $EUID -ne 0 ]]; then
  if command -v sudo >/dev/null 2>&1; then
    echo "需要管理员权限，正在用 sudo 重新运行…"
    exec sudo -E bash "$0" "$@"
  fi
  die "需要 root 权限，请用 sudo 运行。"
fi

cat <<EOF

${B}X30 遥控系统 安装程序${N}
${DIM}源码目录 $SRC${N}
EOF

# ===========================================================================
step 1 "环境检查"
# ===========================================================================

MISSING=""
for tool in cmake g++ make python3; do
  command -v "$tool" >/dev/null 2>&1 || MISSING="$MISSING $tool"
done

if [[ -n $MISSING ]]; then
  warn "缺少工具：$MISSING"
  if ping -c1 -W2 mirrors.tuna.tsinghua.edu.cn >/dev/null 2>&1 ||
     ping -c1 -W2 8.8.8.8 >/dev/null 2>&1; then
    if ask_yn "检测到能上网，现在自动安装？" y; then
      DEBIAN_FRONTEND=noninteractive apt-get update -qq
      DEBIAN_FRONTEND=noninteractive apt-get install -y \
        build-essential cmake python3 || die "装包失败，请手动处理后重试。"
      ok "工具链已装好"
    else
      die "缺工具装不了。手动执行： apt install build-essential cmake python3"
    fi
  else
    echo
    echo "  板子现在没有网，装不了这些包。两个办法："
    echo "    1. 插网线或用手机 USB 共享网络，然后重跑本程序"
    echo "    2. 在别的机器上下好 deb 包拷过来 dpkg -i"
    die "缺少编译工具：$MISSING"
  fi
else
  ok "编译工具齐全（cmake / g++ / make / python3）"
fi

FREE=$(df -m "$SRC" 2>/dev/null | awk 'NR==2{print $4}')
if [[ -n ${FREE:-} && $FREE -lt 300 ]]; then
  die "磁盘只剩 ${FREE}MB，编译需要约 300MB。先清理一下。"
fi
ok "磁盘剩余 ${FREE:-?}MB"

if grep -qi "rk3588\|rockchip" /proc/device-tree/compatible 2>/dev/null; then
  ok "确认是 RK3588 平台"
else
  warn "没检测到 RK3588，仍会继续（调优项在任何 arm64 Linux 上都适用）"
fi

# ===========================================================================
step 2 "系统调优"
# ===========================================================================

# bootstrap 改的是 CPU 调度器、休眠、自动升级这些系统级设置，做过就不用再做。
# 用 cpu-performance.service 在不在来判断，比记一个标记文件可靠。
NEED_REBOOT=0
if [[ $SKIP_BOOTSTRAP -eq 1 ]]; then
  info "按要求跳过"
elif systemctl is-enabled cpu-performance.service >/dev/null 2>&1; then
  ok "系统调优此前已完成，跳过"
  info "要强制重做的话：sudo bash deploy/bootstrap.sh"
else
  echo "  将锁定 CPU 调度器、屏蔽休眠、关闭自动升级、加大网络缓冲。"
  info "默认的 ondemand 调度器唤醒要几十毫秒，在 50 Hz 控制回路里"
  info "会变成机身肉眼可见的晃动，所以这一步不建议跳过。"
  if ask_yn "现在执行？" y; then
    bash "$SRC/deploy/bootstrap.sh" --skip-packages || die "系统调优失败。"
    ok "系统调优完成"
    NEED_REBOOT=1
  else
    warn "已跳过。控制回路可能有抖动。"
  fi
fi

# ===========================================================================
step 3 "编译"
# ===========================================================================

echo "  编译中，约十几秒…"
if ! cmake -S "$SRC/rk3588" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release \
     > /tmp/x30-cmake.log 2>&1; then
  tail -20 /tmp/x30-cmake.log
  die "cmake 配置失败，完整日志 /tmp/x30-cmake.log"
fi
if ! cmake --build "$SRC/build" -j"$(nproc)" > /tmp/x30-build.log 2>&1; then
  tail -30 /tmp/x30-build.log
  die "编译失败，完整日志 /tmp/x30-build.log"
fi
ok "编译完成 → build/x30_gateway"

# 自测很快，但能挡住"装完才发现二进制是坏的"这类事。
if [[ -f "$SRC/tools/smoke_test.sh" ]]; then
  if bash "$SRC/tools/smoke_test.sh" > /tmp/x30-smoke.log 2>&1; then
    ok "运动控制自测通过"
  else
    warn "自测没过，日志 /tmp/x30-smoke.log"
    ask_yn "仍要继续安装？" n || die "已中止。"
  fi
fi

# ===========================================================================
step 4 "参数确认"
# ===========================================================================

echo "  直接回车用默认值。"
echo
ask "运动主机 IP（所有行走控制）" "$ROBOT_IP";      ROBOT_IP=$REPLY_VAL
ask "感知主机 IP（上下楼、点云、机身相机）" "$PERCEPTION_IP"
PERCEPTION_IP=$REPLY_VAL
ask "遥控服务端口" "$HTTP_PORT";                    HTTP_PORT=$REPLY_VAL

# 监听地址。有 4G 时绑 0.0.0.0 等于把一个无鉴权的控制口挂到广域网上，
# 所以检测到广域接口就强烈提示，而不是等装完再由体检来报警。
WAN=$(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' |
      grep -E '^(ppp|wwan|usb)' | tr '\n' ' ')
if [[ -z $BIND_ADDR ]]; then
  echo
  if [[ -n $WAN ]]; then
    warn "检测到广域接口（$WAN）"
    info "本协议没有身份认证，绑 0.0.0.0 意味着凡能连上此端口者皆可控制机器狗。"
    info "建议填遥控链路（MESH 侧）那块网卡的地址。现在还没接就先留 0.0.0.0，"
    info "等 MESH 到位后重跑本程序收紧。"
  else
    info "监听地址：现在还没接遥控链路的话留 0.0.0.0 便于调试，"
    info "等 MESH 到位后重跑本程序，改成 MESH 侧网卡地址。"
  fi
  ask "监听地址" "0.0.0.0"; BIND_ADDR=$REPLY_VAL
fi

# 点云。默认关，因为感知主机的 ROS 可达性通常还没验证过，
# 开机就去连一台没验过的生产设备不合适。
echo
if [[ $CLOUD == no ]]; then
  if curl -s --max-time 3 "http://$PERCEPTION_IP:11311/" >/dev/null 2>&1; then
    ok "感知主机的 ROS master 有响应，点云可以开"
    ask_yn "开启点云回传？" y && CLOUD=yes
  else
    info "感知主机 $PERCEPTION_IP:11311 的 ROS master 暂无响应，点云先关着。"
    info "确认 roscore 起来后重跑本程序即可打开。"
  fi
fi

if [[ $CLOUD == yes && -z $ROS_HOST ]]; then
  # 感知主机要靠这个地址反连回来送数据，必须是与狗直连那块网卡的地址。
  # 填错的症状是"订阅成功但一帧都收不到"，现场极难查，所以这里自动探。
  GUESS=$(ip -4 route get "$PERCEPTION_IP" 2>/dev/null |
          awk '{for(i=1;i<=NF;i++) if($i=="src") print $(i+1)}' | head -1)
  echo
  if [[ -n $GUESS ]]; then
    info "本机通往感知主机走的是 $GUESS，点云回传地址应当填它。"
  else
    warn "探不到通往感知主机的本机地址，请手工填与狗直连那块网卡的地址。"
  fi
  ask "本机在 ROS 网络中的地址" "${GUESS:-192.168.1.120}"
  ROS_HOST=$REPLY_VAL
fi
[[ -z $ROS_HOST ]] && ROS_HOST="192.168.1.120"

cat <<EOF

  ${B}即将安装：${N}
    运动主机   $ROBOT_IP
    感知主机   $PERCEPTION_IP
    服务端口   $HTTP_PORT
    监听地址   $BIND_ADDR
    点云       $CLOUD$([[ $CLOUD == yes ]] && echo "（本机 ROS 地址 $ROS_HOST）")
EOF
echo
ask_yn "确认？" y || die "已取消，没有改动任何东西。"

# ===========================================================================
step 5 "安装服务"
# ===========================================================================

INSTALL_ARGS=(--robot-ip "$ROBOT_IP" --perception-ip "$PERCEPTION_IP"
              --port "$HTTP_PORT" --bind "$BIND_ADDR")
[[ $CLOUD == yes ]] && INSTALL_ARGS+=(--cloud --ros-host "$ROS_HOST")

if bash "$SRC/deploy/install.sh" "${INSTALL_ARGS[@]}" > /tmp/x30-install.log 2>&1
then
  ok "服务已安装并启动"
else
  tail -30 /tmp/x30-install.log
  die "安装失败，完整日志 /tmp/x30-install.log"
fi

# 桌面快捷方式。日常真正会反复用到的是这三个，放桌面上比记命令实在。
DESK_USER=$(logname 2>/dev/null || echo "${SUDO_USER:-}")
if [[ -n $DESK_USER ]]; then
  DESK_HOME=$(getent passwd "$DESK_USER" | cut -d: -f6)
  for d in "$DESK_HOME/桌面" "$DESK_HOME/Desktop"; do
    [[ -d $d ]] || continue
    make_shortcut() {
      cat > "$d/$1.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$1
Exec=$2
Terminal=$3
Icon=utilities-terminal
EOF
      chmod +x "$d/$1.desktop"
      chown "$DESK_USER" "$d/$1.desktop" 2>/dev/null || true
      # GNOME 需要显式标记信任，否则双击只提示"不受信任的启动器"
      if command -v gio >/dev/null 2>&1; then
        sudo -u "$DESK_USER" gio set "$d/$1.desktop" \
             metadata::trusted true 2>/dev/null || true
      fi
    }

    # Exec= 里塞带引号的复合命令是不可移植的：desktop 规范有自己一套转义，
    # 各家文件管理器解析还不一致。所以复合动作一律先落成脚本，Exec 只指路径。
    HELPER="$SRC/deploy/desktop-checkup.sh"
    cat > "$HELPER" <<EOF
#!/usr/bin/env bash
cd "$SRC" || exit 1
bash deploy/checkup.sh
echo
read -r -p "按回车关闭…" _
EOF
    chmod +x "$HELPER"

    make_shortcut "X30 控制台" "xdg-open http://127.0.0.1:$HTTP_PORT/" false
    make_shortcut "X30 体检"   "$HELPER" true
    make_shortcut "X30 日志"   "journalctl -u x30-gateway -f" true
    ok "桌面快捷方式已生成（控制台 / 体检 / 日志）"
    break
  done
fi

# ===========================================================================
step 6 "体检"
# ===========================================================================

echo
bash "$SRC/deploy/checkup.sh"
CHECK_RC=$?

# ===========================================================================

IP=$(hostname -I 2>/dev/null | awk '{print $1}')
cat <<EOF

${B}══════════════════════════════════════════${N}

  控制台   ${B}http://${IP:-<板子IP>}:${HTTP_PORT}/${N}
  体检     bash deploy/checkup.sh
  日志     journalctl -u x30-gateway -f
  重启     systemctl restart x30-gateway

  以后改地址、端口、点云开关不用再上命令行，控制台右上角有「设置」。
  它要管理令牌，就是下面这串（也可以随时用 sudo bash deploy/checkup.sh --token 取）：

    ${B}$(bash "$SRC/deploy/checkup.sh" --token 2>/dev/null || echo '（取不到，用 sudo bash deploy/checkup.sh --token）')${N}

EOF

if [[ $NEED_REBOOT -eq 1 ]]; then
  echo "  ${Y}系统调优的部分改动（CPU 调度器、休眠屏蔽）重启后才完全生效。${N}"
  if ask_yn "现在重启？" n; then reboot; fi
  echo
fi

if [[ $CHECK_RC -ne 0 ]]; then
  echo "  体检有未通过项，按上面的提示逐条处理，然后重跑："
  echo "      bash deploy/checkup.sh"
  echo
fi

exit 0
