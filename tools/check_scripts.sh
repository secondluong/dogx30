#!/usr/bin/env bash
# 静态检查所有 shell 脚本：语法、CRLF 残留，有 shellcheck 就一并跑。
#
#   bash tools/check_scripts.sh

cd "$(dirname "$0")/.." || exit 1
RC=0

for f in deploy/*.sh tools/*.sh; do
  [[ -e "$f" ]] || continue

  if bash -n "$f" 2>/tmp/synerr; then
    echo "  [OK]   语法 $f"
  else
    echo "  [FAIL] 语法 $f"
    cat /tmp/synerr
    RC=1
  fi

  # CRLF 会让 bash 把 \r 当成命令的一部分，报错信息极具误导性。
  if grep -qU $'\r' "$f" 2>/dev/null; then
    echo "  [FAIL] $f 含 CRLF，需转成 LF"
    RC=1
  fi
done

# 脚本以外的配置文件同样怕 CR，而且症状更隐蔽。systemd 单元带 CR 时，
# 反斜杠续行会断，或者 \r 混进参数里变成 --ros-host "192.168.1.120\r"。
# 这个坑真踩过：当时只查了 *.sh，.service 就这么漏过去了。
echo
for f in deploy/*.service deploy/*.yml deploy/*.json tools/*.py web/*.js; do
  [[ -e "$f" ]] || continue
  if grep -qU $'\r' "$f" 2>/dev/null; then
    echo "  [FAIL] $f 含 CRLF，需转成 LF（跑 python3 tools/fix_eol.py）"
    RC=1
  fi
done
echo "  [OK]   配置文件行尾检查完毕"

if command -v shellcheck >/dev/null 2>&1; then
  echo
  for f in deploy/*.sh tools/*.sh; do
    [[ -e "$f" ]] || continue
    shellcheck -x -S warning "$f" || RC=1
  done
else
  echo
  echo "  未装 shellcheck，跳过深度检查（apt install shellcheck）"
fi

echo
[[ $RC -eq 0 ]] && echo "全部通过" || echo "存在问题"
exit $RC
