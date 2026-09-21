#!/usr/bin/env bash
# 板上装 MediaMTX：把布控球 192.168.10.168 转成平板能拉的 rtsp://192.168.10.120:8554/ptz_vis_main。
# 平板在 10 网，和球同一段，但出口仍走板子，方便码率控制和 WebRTC。
set -euo pipefail

PREFIX="${PREFIX:-/opt/x30}"
VER="${MEDIAMTX_VER:-v1.20.1}"
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

fetch() {
  local url="$1" dest="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --connect-timeout 20 --retry 2 "$url" -o "$dest"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --timeout=20 -O "$dest" "$url"
  else
    return 127
  fi
}

if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
  echo "板上没有 curl/wget，先装下载工具…"
  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -y && apt-get install -y curl wget
  else
    echo "请先安装 curl 或 wget，或把 $FILE 拷到板子。" >&2
    exit 1
  fi
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "下载 MediaMTX ${VER} …"
ok=0
for URL in "${URLS[@]}"; do
  echo "  $URL"
  if fetch "$URL" "$TMP/mtx.tgz"; then
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
  # 旧文件常把 RTSP 绑在 127.0.0.1，平板从 10.2 永远拉不到。
  if grep -qE 'rtspAddress:[[:space:]]*127\.0\.0\.1' "$PREFIX/mediamtx.yml"; then
    sed -i 's/rtspAddress:[[:space:]]*127\.0\.0\.1:8554/rtspAddress: :8554/' \
      "$PREFIX/mediamtx.yml"
    echo "已把 rtspAddress 从 127.0.0.1 改成全部网卡"
  fi
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
echo "  rtsp://192.168.10.120:8554/ptz_vis_main"
ss -lnt | grep -E '8554|8889' || true
