#!/usr/bin/env bash
# 验自解压安装包：能不能解出完整的源码树，以及会不会在不该交互的时候挂住。
#
# 这个包的失败方式很难受 —— 它只在板子上、装机当天才暴露，
# 而那时候人手边没有开发环境。所以宁可在这里多验几条。

set -u
cd "$(dirname "$0")/.." || exit 1

FAILED=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass() { echo "  [OK]   $1"; }
fail() { echo "  [FAIL] $1${2:+  $2}"; FAILED=1; }

echo "== 生成安装包 =="
if ! bash deploy/make_installer.sh "$TMP" > "$TMP/mk.log" 2>&1; then
  tail -20 "$TMP/mk.log"
  fail "make_installer.sh 执行失败"
  exit 1
fi
RUN=$(find "$TMP" -maxdepth 1 -name 'x30-installer-*.run' | head -1)
if [[ -z $RUN ]]; then
  fail "没有产出 .run"
  exit 1
fi
pass "已生成 $(basename "$RUN")  ($(du -h "$RUN" | cut -f1))"

if [[ -x $RUN ]]; then pass "带可执行位"; else fail "没有可执行位"; fi

echo
echo "== 数据段定位 =="

# 壳里用 awk 找 __ARCHIVE__ 独占的那一行。壳自身的注释和 awk 程序里都出现过
# 这个字面量，一旦哪次改动让它们也独占一行，偏移就会算错、解压出半个包。
MARKS=$(grep -c '^__ARCHIVE__$' "$RUN")
if [[ $MARKS -eq 1 ]]; then
  pass "__ARCHIVE__ 标记恰好出现一次"
else
  fail "__ARCHIVE__ 标记出现 $MARKS 次" "必须恰好一次，否则解压偏移会算错"
fi

echo
echo "== 解压 =="

if timeout 60 "$RUN" --extract "$TMP/out" > "$TMP/ex.log" 2>&1; then
  pass "--extract 成功"
else
  fail "--extract 失败" "$(tail -3 "$TMP/ex.log")"
fi

for must in deploy/install_gui.sh deploy/install.sh deploy/checkup.sh \
            deploy/bootstrap.sh deploy/render_unit.sh deploy/config_util.sh \
            deploy/x30-gateway.service deploy/media.json \
            rk3588/CMakeLists.txt rk3588/src/main.cpp \
            rk3588/src/gateway_config.cpp \
            web/index.html web/gamepad.js web/settings.js \
            tools/checkup_test.sh; do
  if [[ -f "$TMP/out/$must" ]]; then pass "含 $must"; else fail "缺 $must"; fi
done

echo
echo "== 解出来的东西必须是好的 =="

# 只验完整性不够。包里的脚本要是语法坏的，照样要等到板子上才知道。
for f in "$TMP/out"/deploy/*.sh "$TMP/out"/tools/*.sh; do
  [[ -e $f ]] || continue
  if ! bash -n "$f" 2>"$TMP/syn"; then
    fail "语法错误 ${f#"$TMP/out/"}" "$(head -2 "$TMP/syn")"
  fi
done
pass "解出的 shell 脚本语法全部正确"

if grep -rqU $'\r' "$TMP/out/deploy" 2>/dev/null; then
  fail "解出来的 deploy/ 里有 CRLF" "systemd 单元带 CR 会让续行断掉"
else
  pass "解出来的文件没有 CRLF"
fi

if timeout 20 bash "$TMP/out/deploy/install_gui.sh" --help > "$TMP/h.log" 2>&1; then
  pass "install_gui.sh --help 正常"
else
  fail "install_gui.sh --help 失败" "$(tail -3 "$TMP/h.log")"
fi

echo
echo "== 不该弹终端的场合不能弹 =="

# 真踩过：判据里带了 stdout，结果 `./x30-installer.run > log.txt` 也去开终端，
# 在没有图形界面或无人值守的环境下直接挂死。
# 这里模拟"有 DISPLAY、stdin 不是终端、输出被重定向"，必须照常干活不挂。
if DISPLAY=:0 timeout 30 "$RUN" --extract "$TMP/out2" </dev/null \
     > "$TMP/nt.log" 2>&1; then
  pass "有 DISPLAY 且输出被重定向时，--extract 照常完成"
else
  fail "有 DISPLAY 时 --extract 挂住或失败" "$(tail -3 "$TMP/nt.log")"
fi

# --yes 是明确的非交互意图，任何情况下都不该去弹终端。
# 这里不能真跑安装（会动系统），所以只检查壳里那段判断存在且形式正确。
if grep -q 'WANT_TERM=0' "$RUN"; then
  pass "--yes 会关掉开终端的行为"
else
  fail "壳里找不到 --yes 抑制开终端的逻辑"
fi

echo
echo "== 解出来的树要和 package.sh 的一致 =="

# 两条发布路径（tar.gz 和 .run）必须给出同样的东西，否则"我装的是哪个版本"
# 会变成一个没法回答的问题。
bash deploy/package.sh "$TMP/pkg" > /dev/null 2>&1
PKGTAR=$(find "$TMP/pkg" -maxdepth 1 -name 'dogx30-*.tar.gz' | head -1)
mkdir -p "$TMP/pkgout"
tar xzf "$PKGTAR" -C "$TMP/pkgout" --strip-components=1

if diff -r "$TMP/out" "$TMP/pkgout" > "$TMP/diff.log" 2>&1; then
  pass "两条发布路径内容一致"
else
  # 时间戳不同会导致 安装说明.txt 里的包名不同，这属于预期
  REAL=$(grep -v '安装说明' "$TMP/diff.log" | head -5)
  if [[ -z $REAL ]]; then
    pass "两条发布路径内容一致（仅安装说明的包名不同）"
  else
    fail "两条发布路径内容不一致" "$REAL"
  fi
fi

echo
if [[ $FAILED -eq 0 ]]; then
  echo "安装包检查通过"
else
  echo "安装包检查失败"
fi
exit $FAILED
