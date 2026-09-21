#!/usr/bin/env bash
# 板上装 MediaMTX：把布控球 192.168.1.168 转成平板能拉的 rtsp://192.168.10.2:8554/ptz_vis_main。
# 平板在 10 网，打不到 1 网的球，所以这一步不能省。
set -euo pipefail

PREFIX="${PREFIX:-/opt/x30}"
VER="${MEDIAMTX_VER:-v1.11.3}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
ARCH=linux_arm64
FILE="mediamtx_${VER}_${ARCH}.tar.gz"
URLS=(
  "https://github.com/bluenviron/mediamtx/releases/download/${VER}/${FILE}"
  "https://ghfast.top/https://github.com/bluenviron/mediamtx/releases/download/${VER}/${FILE}"
)

if [[ "$(id -u)" -ne 0 ]]; then
  echo "请用 root 跑：sudo $0" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "下载 MediaMTX ${VER} …"
ok=0
for URL in "${URLS[@]}"; do
  echo "  $URL"
  if curl -fL --connect-timeout 20 --retry 2 "$URL" -o "$TMP/mtx.tgz"; then
    ok=1
    break
  fi
done
if [[ "$ok" -ne 1 ]]; then
  echo "下载失败。板子要能上外网，或把 $FILE 拷到板子后：" >&2
  echo "  tar -xzf $FILE && install -m 755 mediamtx /opt/x30/bin/mediamtx" >&2
  exit 1
fi
tar -xzf "$TMP/mtx.tgz" -C "$TMP"
if [[ ! -x "$TMP/mediamtx" ]]; then
  echo "压缩包里没有 mediamtx 可执行文件" >&2
  exit 1
fi

install -d "$PREFIX/bin"
install -m 755 "$TMP/mediamtx" "$PREFIX/bin/mediamtx"
if [[ ! -f "$PREFIX/mediamtx.yml" ]]; then
  install -m 644 "$SRC/deploy/mediamtx.yml" "$PREFIX/mediamtx.yml"
else
  echo "保留已有 $PREFIX/mediamtx.yml"
fi

sed -e "s|/opt/x30|$PREFIX|g" "$SRC/deploy/x30-media.service" \
  > /etc/systemd/system/x30-media.service
systemctl daemon-reload
systemctl enable x30-media >/dev/null
systemctl restart x30-media
sleep 1

if ! systemctl is-active --quiet x30-media; then
  echo "x30-media 没起来：" >&2
  journalctl -u x30-media -n 30 --no-pager >&2
  exit 1
fi

echo "MediaMTX 已启动。平板拉双光："
echo "  rtsp://192.168.10.2:8554/ptz_vis_main"
ss -lnt | grep -E '8554|8889' || true
