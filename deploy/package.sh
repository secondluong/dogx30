#!/usr/bin/env bash
# 打一个可离线拷贝到 RK3588 的源码包。
#
#   bash deploy/package.sh [输出目录]
#
# 为什么需要它：板子首次上电时多半还没配好网络，git clone 走不通。
# 打成一个 tar.gz 用 U 盘或 scp 送过去，是最不会出岔子的路径。
#
# 包里只有源码和脚本，不含 build 产物 —— 交叉编译出来的二进制未必能在板子上跑，
# 一律在板子上现编，反正整个工程只依赖编译器，十几秒就好。

set -euo pipefail

cd "$(dirname "$0")/.." || exit 1
SRC=$(pwd)
OUT_DIR="${1:-$SRC/dist}"
STAMP=$(date +%Y%m%d-%H%M)
NAME="dogx30-$STAMP"
STAGE="$OUT_DIR/$NAME"

echo "源码目录 : $SRC"
echo "输出目录 : $OUT_DIR"
echo

rm -rf "$STAGE"
mkdir -p "$STAGE"

# 逐项拷贝而不是排除法：漏带文件的后果（板子上编不过）比多带文件严重得多，
# 但用排除法容易把 build/ 之类的东西带进去，所以宁可显式列清单。
for item in rk3588 web deploy tools docs android-app README.md; do
  if [[ ! -e "$SRC/$item" ]]; then
    echo "缺少 $item，打包中止。"
    exit 1
  fi
  cp -r "$SRC/$item" "$STAGE/"
done

# 清掉可能混进去的构建产物与缓存。
rm -rf "$STAGE/rk3588/build" "$STAGE/build"
rm -rf "$STAGE/android-app/build" "$STAGE/android-app/app/build" \
       "$STAGE/android-app/.gradle"
find "$STAGE" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true
find "$STAGE" -name '*.pyc' -delete 2>/dev/null || true

# Windows 上编辑过的脚本可能带 CRLF，在板子上会报 "command not found"。
# 这个坑之前踩过，打包时统一规范化。
find "$STAGE" \( -name '*.sh' -o -name '*.py' \) -type f -print0 \
  | xargs -0 sed -i 's/\r$//' 2>/dev/null || true

cat > "$STAGE/安装说明.txt" <<'EOF'
最省事的办法是一条命令走完全程：

  sudo bash deploy/install_gui.sh

它会依次做环境检查、系统调优、编译、参数确认、装服务、体检，
中途问几个参数（都有默认值，直接回车即可），做过的步骤自动跳过。
加 --yes 就全用默认值不提问。

也可以手工分步（第一次这样装能看清每步在干什么）：

  sudo bash deploy/bootstrap.sh                    # 系统调优，装编译器（只需一次）
  sudo bash deploy/install.sh \
      --robot-ip 192.168.1.103 \
      --perception-ip 192.168.1.105 \
      --port 8080

  bash deploy/checkup.sh                           # 体检：一次性查清整条链路

装完访问 http://<板子IP>:8080/

若本机接有 4G 等广域接口，务必加 --bind <遥控链路地址>，
否则遥控端口会暴露在广域网上（协议尚无身份认证）。

体检脚本是只读的，可以反复跑。每一项要么过，要么直接告诉你下一步做什么，
不用翻文档对症状。装机当天先跑它，比逐项手动排查快得多。

完整步骤见 docs/rk3588-setup.md。
EOF

mkdir -p "$OUT_DIR"
tar -czf "$OUT_DIR/$NAME.tar.gz" -C "$OUT_DIR" "$NAME"
rm -rf "$STAGE"

SIZE=$(du -h "$OUT_DIR/$NAME.tar.gz" | cut -f1)
cat <<EOF

打包完成: $OUT_DIR/$NAME.tar.gz  ($SIZE)

拷到板子上后：
  tar xzf $NAME.tar.gz && cd $NAME
  sudo bash deploy/bootstrap.sh
  sudo bash deploy/install.sh --robot-ip 192.168.1.103
  bash deploy/checkup.sh
EOF
