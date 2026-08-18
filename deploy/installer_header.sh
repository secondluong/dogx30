#!/bin/bash
# X30 遥控系统 —— 自解压安装包
#
# 这段是壳，真正的源码以 tar.gz 形式附在文件末尾（__ARCHIVE__ 标记之后）。
# 由 deploy/make_installer.sh 生成，不要直接改这个文件的产物。
#
#   sudo ./x30-installer-<日期>.run          交互安装
#   sudo ./x30-installer-<日期>.run --yes    全默认，不问
#   ./x30-installer-<日期>.run --extract 目录  只解压不安装
#
# 在桌面上双击也可以：本壳会自己找一个终端模拟器把自己重新跑起来。

set -u

SELF=$(readlink -f "$0")

# --- 找到附加的归档 ---------------------------------------------------------

ARCHIVE_LINE=$(awk '/^__ARCHIVE__$/ { print NR + 1; exit 0; }' "$SELF")
if [[ -z ${ARCHIVE_LINE:-} ]]; then
  echo "安装包损坏：找不到数据段。请重新拷贝一份。"
  exit 1
fi

# --extract 只解压不安装，方便先看看里面是什么，或者手工分步执行。
# 放在开终端逻辑之前：它不需要交互，重定向到文件也是常见用法。
if [[ ${1:-} == "--extract" ]]; then
  DEST=${2:-./x30-src}
  mkdir -p "$DEST"
  tail -n +"$ARCHIVE_LINE" "$SELF" | tar xz -C "$DEST" --strip-components=1
  echo "已解压到 $DEST"
  echo "手工安装： cd $DEST && sudo bash deploy/install_gui.sh"
  exit 0
fi

# --- 双击进来的情况 ---------------------------------------------------------
#
# 文件管理器执行 .run 时通常不给终端，安装过程既要提问也要显示进度，
# 没有终端的话用户只会看到"双击了但什么都没发生"。所以自己找一个终端重新跑。
#
# 判据只看 stdin：能不能提问取决于它。**不要把 stdout 也算进来** ——
# `sudo ./x30-installer.run > log.txt` 是完全正常的用法，
# 那种情况下弹一个终端出来纯属捣乱（而且在无人值守的场合会直接挂住）。
# 显式要求非交互（--yes）时同样不弹。
WANT_TERM=1
for a in "$@"; do
  [[ $a == "-y" || $a == "--yes" ]] && WANT_TERM=0
done

if [[ $WANT_TERM -eq 1 ]] && [[ ! -t 0 ]] && [[ -z ${X30_IN_TERMINAL:-} ]] &&
   [[ -n ${DISPLAY:-}${WAYLAND_DISPLAY:-} ]]; then

  # 写一个小脚本再让终端去执行它，而不是把命令拼成字符串传进 -e。
  # 各家终端对 -e 后面那一坨的解析方式互不相同，拼字符串必然在某台机器上炸。
  LAUNCH=$(mktemp /tmp/x30-launch-XXXXXX.sh)
  {
    echo '#!/bin/bash'
    printf 'X30_IN_TERMINAL=1 "%s"' "$SELF"
    for a in "$@"; do printf ' "%s"' "$a"; done
    echo
    echo 'echo'
    echo 'read -r -p "按回车关闭…" _'
    printf 'rm -f "%s"\n' "$LAUNCH"
  } > "$LAUNCH"
  chmod +x "$LAUNCH"

  for term in x-terminal-emulator gnome-terminal xfce4-terminal lxterminal \
              mate-terminal konsole qterminal terminator xterm; do
    command -v "$term" >/dev/null 2>&1 || continue
    case "$term" in
      # 新版 gnome-terminal 去掉了 -e，只认 -- 后面跟命令
      gnome-terminal) exec "$term" -- "$LAUNCH" ;;
      *)              exec "$term" -e "$LAUNCH" ;;
    esac
  done

  rm -f "$LAUNCH"
  # 一个终端都找不到就继续往下跑，至少输出还能重定向出去
fi

# 解压到 /tmp 而不是当前目录：安装包常常是从 U 盘直接运行的，
# 而 FAT32 上没法保留可执行位，编译也会慢得多。
WORK=$(mktemp -d /tmp/x30-install-XXXXXX)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

echo "正在解包…"
if ! tail -n +"$ARCHIVE_LINE" "$SELF" | tar xz -C "$WORK" --strip-components=1
then
  echo "解包失败。文件可能在拷贝过程中损坏，请重新拷贝。"
  exit 1
fi

if [[ ! -f "$WORK/deploy/install_gui.sh" ]]; then
  echo "安装包内容不完整，缺 deploy/install_gui.sh。"
  exit 1
fi

# 源码留一份到 ~ 下。install.sh 只把二进制和 web 装到 /opt/x30，
# 而 checkup.sh、rtsp_probe.py 这些排查工具都在源码树里，
# 装完就把 /tmp 删掉的话，出问题时手边反而没有工具可用。
KEEP_USER=$(logname 2>/dev/null || echo "${SUDO_USER:-root}")
KEEP_HOME=$(getent passwd "$KEEP_USER" 2>/dev/null | cut -d: -f6)
KEEP_HOME=${KEEP_HOME:-/root}
KEEP="$KEEP_HOME/dogx30"

# 旧的挪成 .bak 而不是直接删。这是别人的家目录，万一里面有现场改过的东西，
# 删掉就找不回来了；留一份的成本只是几百 KB。
if [[ -d $KEEP ]]; then
  rm -rf "$KEEP.bak"
  mv "$KEEP" "$KEEP.bak"
  echo "原有的 $KEEP 已挪到 $KEEP.bak"
fi
mkdir -p "$KEEP"
cp -r "$WORK/." "$KEEP/"
chown -R "$KEEP_USER" "$KEEP" 2>/dev/null || true
echo "源码已放到 $KEEP"

# exec 之后 trap 不会执行，所以在这里先手工清掉临时目录
cleanup
trap - EXIT

cd "$KEEP" || exit 1
# shellcheck disable=SC2093  # 有意替换进程，下面只剩数据段
exec bash deploy/install_gui.sh "$@"

# 下面是数据段，不要编辑。
__ARCHIVE__
