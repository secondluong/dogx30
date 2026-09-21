#!/usr/bin/env bash
# 同一只网口保留狗身 1 网地址，再挂一个 10 网地址给 MESH/平板。
# 2.4G 射频占着 192.168.1.0/24，遥控 WiFi 不能再和它同段。
#
# 用法（在 RK3588 上，网线已插主板交换机）：
#   sudo bash deploy/add_remote_ip.sh
#   sudo bash deploy/add_remote_ip.sh eth0 192.168.10.2/24
#
# 不改狗、球、气体的 IP。MESH 电台和平板改到 10 网，App 填 192.168.10.2。

set -euo pipefail

IFACE="${1:-}"
ADDR="${2:-192.168.10.2/24}"

if [[ -z "$IFACE" ]]; then
  IFACE=$(ip -br link | awk '/^e/{print $1; exit}')
fi
if [[ -z "$IFACE" ]] || ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "找不到网卡。当前："
  ip -br link
  exit 1
fi

DOG_NET="192.168.1.0/24"
MESH_NET="192.168.10.0/24"
DOG_SRC=$(ip -4 -o addr show dev "$IFACE" | awk '{print $4}' | awk -F/ -v n="$DOG_NET" '
  BEGIN { split(n, a, "/") }
  $1 ~ /^192\.168\.1\./ { print $1; exit }
')
MESH_SRC="${ADDR%%/*}"

if ip -4 addr show dev "$IFACE" | grep -q "inet ${MESH_SRC}/"; then
  echo "  $IFACE 上已有 $ADDR"
else
  ip addr add "$ADDR" dev "$IFACE"
  echo "  已给 $IFACE 加上 $ADDR"
fi

# 两段地址挂同一张网卡时，内核可能用 10.2 去打狗。运动主机只给
# network.toml 里登记的 1 网地址回遥测，于是 App 上就「已连接 / 狗未接通」来回闪。
if [[ -n "$DOG_SRC" ]]; then
  ip route replace "$DOG_NET" dev "$IFACE" src "$DOG_SRC"
  echo "  去 1 网的包强制源地址 $DOG_SRC"
fi
ip route replace "$MESH_NET" dev "$IFACE" src "$MESH_SRC"
echo "  去 10 网的包强制源地址 $MESH_SRC"

persist() {
  if command -v nmcli >/dev/null 2>&1 && systemctl is-active --quiet NetworkManager; then
    local conn
    conn=$(nmcli -t -f NAME,DEVICE connection show --active | awk -F: -v d="$IFACE" '$2==d{print $1; exit}')
    [[ -z "$conn" ]] && conn="x30-payload"
    nmcli connection modify "$conn" +ipv4.addresses "$ADDR" ipv4.never-default yes 2>/dev/null || \
      nmcli connection modify "$conn" ipv4.addresses "$(ip -4 -o addr show dev "$IFACE" | awk '{print $4}' | tr '\n' ',' | sed 's/,$//')" ipv4.never-default yes
    # 不要 nmcli connection up：会整卡重拨，App 上看就是连上又断。
    echo "  已写入 NetworkManager 连接 $conn（未重拨）"
    return
  fi
  if [[ -d /etc/netplan ]]; then
    echo "  请把 $ADDR 加进 /etc/netplan 里 $IFACE 的 addresses 列表后执行 netplan apply"
    return
  fi
  echo "  本次已生效。要开机仍在，把 $ADDR 写进该网卡的网络配置。"
}

persist
echo
ip -br addr show "$IFACE"
echo
echo "下一步：MESH 狗端/手持改到 192.168.10.x，App 服务 IP 填 192.168.10.2"
echo "狗 .103、球 .168、板子 1 网地址都不要改。"
