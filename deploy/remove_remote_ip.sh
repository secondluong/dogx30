#!/usr/bin/env bash
# 卸掉同一网口上后挂的 10 网地址。1 网地址、狗、球都不动。
#
#   sudo bash deploy/remove_remote_ip.sh
#   sudo bash deploy/remove_remote_ip.sh eth0 192.168.10.2/24

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

MESH_NET="192.168.10.0/24"
MESH_SRC="${ADDR%%/*}"
DOG_NET="192.168.1.0/24"
DOG_SRC=$(ip -4 -o addr show dev "$IFACE" | awk '{print $4}' | awk -F/ '
  $1 ~ /^192\.168\.1\./ { print $1; exit }
')

ip route del "$MESH_NET" dev "$IFACE" 2>/dev/null && echo "  已删 10 网路由" || true
if ip -4 addr show dev "$IFACE" | grep -q "inet ${MESH_SRC}/"; then
  ip addr del "$ADDR" dev "$IFACE"
  echo "  已从 $IFACE 去掉 $ADDR"
else
  echo "  $IFACE 上没有 $ADDR"
fi

if [[ -n "$DOG_SRC" ]]; then
  ip route replace "$DOG_NET" dev "$IFACE" src "$DOG_SRC"
  echo "  去 1 网的包仍用源地址 $DOG_SRC"
fi

persist() {
  if command -v nmcli >/dev/null 2>&1 && systemctl is-active --quiet NetworkManager; then
    local conn
    conn=$(nmcli -t -f NAME,DEVICE connection show --active | awk -F: -v d="$IFACE" '$2==d{print $1; exit}')
    [[ -z "$conn" ]] && return
    nmcli connection modify "$conn" -ipv4.addresses "$ADDR" 2>/dev/null || true
    echo "  已从 NetworkManager 连接 $conn 去掉 $ADDR（未重拨）"
    return
  fi
  if [[ -d /etc/netplan ]]; then
    echo "  请从 /etc/netplan 里 $IFACE 的 addresses 删掉 $ADDR 后执行 netplan apply"
    return
  fi
  echo "  本次已生效。要开机也不再出现，从该网卡配置里删掉 $ADDR。"
}

persist

CONF="${PREFIX:-/opt/x30}/conf/gateway.conf"
if [[ -f "$CONF" ]] && grep -qE 'bind_address[[:space:]]*=[[:space:]]*192\.168\.10\.2' "$CONF"; then
  sed -i 's/bind_address[[:space:]]*=[[:space:]]*192\.168\.10\.2/bind_address = 0.0.0.0/' "$CONF"
  echo "  网关原先只听 10.2，已改成 0.0.0.0"
  if systemctl is-active --quiet x30-gateway; then
    systemctl restart x30-gateway
    echo "  已重启 x30-gateway"
  fi
fi

echo
ip -br addr show "$IFACE"
echo
echo "板子只留 1 网即可。App 服务 IP 填 192.168.1.101。"
