#!/usr/bin/env bash
# RK3588 系统层准备。
#
#   sudo bash deploy/bootstrap.sh                              # 系统调优
#   sudo bash deploy/bootstrap.sh --static-ip 192.168.1.120/24 # 顺带配静态 IP
#
# 这里做的都是"不做也能跑、但迟早出问题"的事。每一项都写了为什么。
# 幂等，可反复执行。
#
# 完整部署步骤见 docs/rk3588-setup.md。

set -euo pipefail

STATIC_IP=""
IFACE="eth0"
SKIP_PACKAGES=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --static-ip)     STATIC_IP="$2"; shift 2 ;;
    --iface)         IFACE="$2"; shift 2 ;;
    --skip-packages) SKIP_PACKAGES=1; shift ;;
    -h|--help)
      sed -n '2,12p' "$0" | sed 's/^# \?//'
      exit 0 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "需要 root 权限，请用 sudo 运行。"
  exit 1
fi

step() { echo; echo "== $* =="; }

# 提醒而非阻断：本脚本的调优在任何 arm64 Linux 上都适用，
# 只是针对 RK3588 的工况来选的。
if ! grep -qi "rk3588\|rockchip" /proc/device-tree/compatible 2>/dev/null; then
  echo "提示：未检测到 RK3588，脚本仍会继续，但请确认这是目标板卡。"
fi

# ---------------------------------------------------------------------------
step "安装编译工具链"
# ---------------------------------------------------------------------------

if [[ $SKIP_PACKAGES -eq 0 ]]; then
  if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq || echo "apt update 失败（可能暂时没有互联网），继续。"
    apt-get install -y build-essential cmake git python3 \
      || echo "装包失败，请确认网络后手动执行： apt install build-essential cmake git python3"
  else
    echo "非 apt 系统，请自行安装 build-essential / cmake / git / python3。"
  fi
else
  echo "已跳过。"
fi

# ---------------------------------------------------------------------------
step "CPU 调度器锁定 performance"
# ---------------------------------------------------------------------------

# 默认的 ondemand/schedutil 在低负载时降频，唤醒需要几十毫秒。
# 网关的 50 Hz 轴指令回路一旦出现这种抖动，直接表现为机身晃动。
# RK3588 是大小核架构，有多个 policy（A55 一组、A76 两组），要逐个设。
cat > /etc/systemd/system/cpu-performance.service <<'EOF'
[Unit]
Description=锁定 CPU 调度器为 performance（消除遥控回路的延迟抖动）
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done'

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now cpu-performance.service >/dev/null 2>&1 || true

for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
  [[ -e "$g" ]] && echo "  $(dirname "$g" | xargs basename): $(cat "$g")"
done

# ---------------------------------------------------------------------------
step "禁止休眠与自动升级"
# ---------------------------------------------------------------------------

# 载荷计算机在任务中途睡过去，等于机器狗突然失控。
systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target \
  >/dev/null 2>&1 || true
echo "  已屏蔽 sleep/suspend/hibernate"

# apt 在任务中途跑起来会抢 CPU 和 IO，还可能重启服务。
# 升级改为人工在桌面阶段执行。
if [[ -f /etc/apt/apt.conf.d/20auto-upgrades ]]; then
  sed -i 's/"1"/"0"/g' /etc/apt/apt.conf.d/20auto-upgrades
fi
systemctl disable --now unattended-upgrades >/dev/null 2>&1 || true
systemctl disable --now apt-daily.timer apt-daily-upgrade.timer >/dev/null 2>&1 || true
echo "  已关闭自动升级"

# 平时无显示器运行，控制台黑屏只会妨碍现场插上显示器排查。
setterm -blank 0 -powerdown 0 >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
step "关闭 WiFi 省电模式"
# ---------------------------------------------------------------------------

# 省电模式下网卡会周期性进入低功耗，带来几百毫秒级的延迟尖峰。
# 对遥控来说这是致命的 —— 表现为摇杆偶发卡顿、看门狗莫名触发。
mkdir -p /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/wifi-powersave-off.conf <<'EOF'
[connection]
wifi.powersave = 2
EOF
echo "  已写入 NetworkManager 配置（2 = 关闭省电）"

# 有些板子用 wpa_supplicant 而非 NetworkManager，兜一层。
for dev in /sys/class/net/wl*; do
  [[ -e "$dev" ]] || continue
  iw dev "$(basename "$dev")" set power_save off >/dev/null 2>&1 || true
done

# ---------------------------------------------------------------------------
step "网络栈调优"
# ---------------------------------------------------------------------------

# 点云回传是突发大流量（每帧数万点），默认缓冲会丢包。
# 现在先设好，免得视频/点云阶段再排查一次莫名其妙的丢帧。
cat > /etc/sysctl.d/99-x30.conf <<'EOF'
# 点云与视频回传的突发流量需要更大的套接字缓冲
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.core.rmem_default = 1048576
net.core.wmem_default = 1048576

# 遥控是小包高频，加大 backlog 避免突发时排队丢弃
net.core.netdev_max_backlog = 5000
EOF
sysctl -p /etc/sysctl.d/99-x30.conf >/dev/null
echo "  已加大套接字缓冲"

# ---------------------------------------------------------------------------
step "静态 IP"
# ---------------------------------------------------------------------------

if [[ -z "$STATIC_IP" ]]; then
  echo "  未指定 --static-ip，跳过。"
  echo "  上狗前执行： sudo bash deploy/bootstrap.sh --static-ip 192.168.1.120/24 --iface $IFACE"
else
  if ! ip link show "$IFACE" >/dev/null 2>&1; then
    echo "  网卡 $IFACE 不存在。当前网卡："
    ip -br link
    exit 1
  fi

  # 刻意不配默认网关。机器狗内网没有互联网出口，在这里配网关会抢走默认路由，
  # 之后通过 WiFi 上网就不通了，症状是"能 ping 通 IP 但域名解析不了"。
  if command -v nmcli >/dev/null 2>&1 && systemctl is-active --quiet NetworkManager; then
    CONN="x30-payload"
    nmcli connection delete "$CONN" >/dev/null 2>&1 || true
    nmcli connection add type ethernet con-name "$CONN" ifname "$IFACE" \
      ipv4.method manual ipv4.addresses "$STATIC_IP" \
      ipv4.never-default yes ipv6.method ignore \
      connection.autoconnect yes >/dev/null
    nmcli connection up "$CONN" >/dev/null
    echo "  已通过 NetworkManager 配置 $IFACE = $STATIC_IP（不设默认网关）"

  elif [[ -d /etc/netplan ]]; then
    cat > /etc/netplan/99-x30.yaml <<EOF
network:
  version: 2
  renderer: networkd
  ethernets:
    $IFACE:
      dhcp4: no
      addresses: [$STATIC_IP]
EOF
    chmod 600 /etc/netplan/99-x30.yaml
    netplan apply
    echo "  已通过 netplan 配置 $IFACE = $STATIC_IP（不设默认网关）"

  else
    cat > /etc/systemd/network/99-x30.network <<EOF
[Match]
Name=$IFACE

[Network]
Address=$STATIC_IP
EOF
    systemctl enable --now systemd-networkd >/dev/null 2>&1 || true
    systemctl restart systemd-networkd
    echo "  已通过 systemd-networkd 配置 $IFACE = $STATIC_IP（不设默认网关）"
  fi

  sleep 2
  ip -br addr show "$IFACE"
fi

# ---------------------------------------------------------------------------
step "完成"
# ---------------------------------------------------------------------------

cat <<'EOF'

系统层准备就绪。下一步：

  1. 用仿真器自测（还不要接机器狗）
       cmake -S rk3588 -B build -DCMAKE_BUILD_TYPE=Release
       cmake --build build -j$(nproc)
       bash tools/serve_test.sh

  2. 在运动主机上登记本机 IP（不做的话收不到任何遥测）
       见 docs/hardware-integration.md 第四节

  3. 装成系统服务
       sudo bash deploy/install.sh --robot-ip 192.168.1.103

完整步骤见 docs/rk3588-setup.md。

部分改动（CPU 调度器、休眠屏蔽）重启后才完全生效，建议现在重启一次。
EOF
