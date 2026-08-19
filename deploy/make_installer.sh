#!/usr/bin/env bash
# 生成单文件自解压安装包。
#
#   bash deploy/make_installer.sh [输出目录]
#   → dist/x30-installer-<日期>.run
#
# 拷到板子上一条命令装完：
#   sudo ./x30-installer-<日期>.run
#
# 为什么要它：原来的流程是 tar 解压 → bootstrap → 重启 → cmake → make →
# install.sh → checkup，七八条命令。装机当天人蹲在狗旁边，漏一步很正常，
# 而其中几种漏法的现场症状一模一样（都表现为"连上了但没数据"），
# 排查成本远高于做这个包的成本。

set -euo pipefail

cd "$(dirname "$0")/.." || exit 1
SRC=$(pwd)
OUT_DIR="${1:-$SRC/dist}"
STAMP=$(date +%Y%m%d-%H%M)
RUN="$OUT_DIR/x30-installer-$STAMP.run"

HEADER="$SRC/deploy/installer_header.sh"
[[ -f $HEADER ]] || { echo "缺少 $HEADER"; exit 1; }

echo "源码目录 : $SRC"
echo "输出      : $RUN"
echo

# 复用 package.sh 打出的源码包，保证两条路径装出来的东西完全一致。
# 自己再抄一份文件清单的话，迟早会和 package.sh 漂开。
echo "调用 package.sh 打源码包…"
PKG_OUT=$(mktemp -d)
trap 'rm -rf "$PKG_OUT"' EXIT
bash "$SRC/deploy/package.sh" "$PKG_OUT" > /dev/null

TARBALL=$(find "$PKG_OUT" -maxdepth 1 -name 'dogx30-*.tar.gz' | head -1)
[[ -n $TARBALL ]] || { echo "package.sh 没有产出 tar.gz"; exit 1; }

mkdir -p "$OUT_DIR"

# 壳 + 数据段。壳里用 awk 找 __ARCHIVE__ 标记的行号，再用 tail -n +N 取数据，
# 所以标记必须独占一行、且壳内不能再出现同样的字面量。
cat "$HEADER" > "$RUN"
cat "$TARBALL" >> "$RUN"
chmod +x "$RUN"

# 自检：壳能不能真的把数据段取回来。生成一个装不上的包比不生成更糟 ——
# 那要等到板子跟前才发现。
VERIFY=$(mktemp -d)
if ! "$RUN" --extract "$VERIFY" > /dev/null 2>&1; then
  rm -rf "$VERIFY"
  echo "自检失败：生成的包解压不出来。"
  exit 1
fi
for must in deploy/install_gui.sh deploy/install.sh deploy/checkup.sh \
            deploy/config_util.sh rk3588/CMakeLists.txt web/index.html \
            web/settings.js; do
  if [[ ! -f "$VERIFY/$must" ]]; then
    rm -rf "$VERIFY"
    echo "自检失败：包里缺 $must"
    exit 1
  fi
done
FILES=$(find "$VERIFY" -type f | wc -l)
rm -rf "$VERIFY"

SIZE=$(du -h "$RUN" | cut -f1)
cat <<EOF
自检通过（$FILES 个文件）

打包完成: $RUN  ($SIZE)

拷到板子上（U 盘请用 FAT32），然后：

  chmod +x x30-installer-$STAMP.run
  sudo ./x30-installer-$STAMP.run

或者在桌面上双击它 —— 会自己开一个终端跑起来。

其他用法：
  sudo ./x30-installer-$STAMP.run --yes        全用默认值，不提问
  ./x30-installer-$STAMP.run --extract ~/src   只解压不安装
EOF
