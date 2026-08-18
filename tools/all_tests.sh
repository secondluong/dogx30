#!/usr/bin/env bash
# 跑全部本地测试。上板子前和改完代码后都该过一遍。
#
#   bash tools/all_tests.sh

set -u
cd "$(dirname "$0")/.." || exit 1

RC=0
run() {
  local name=$1 script=$2
  echo
  echo "############ $name ############"
  local runner=bash
  case $script in
    *.py) runner=python3 ;;
    *.js) runner=node ;;
  esac
  # 遥控端的测试要 node。板子上不装 node 也能跑其余测试，所以缺了就跳过，
  # 但要说清楚跳过了什么，免得看到"全部通过"以为真的全跑了。
  if ! command -v "$runner" >/dev/null 2>&1; then
    echo ">>> $name 跳过（没有 $runner）"
    return
  fi
  if $runner "$script"; then
    echo ">>> $name 通过"
  else
    echo ">>> $name 失败"
    RC=1
  fi
}

run "shell 脚本静态检查" tools/check_scripts.sh
run "安装参数替换检查"   tools/install_dryrun.sh
run "体检脚本配置解析"   tools/checkup_test.sh
run "自解压安装包检查"   tools/installer_test.sh
run "RTSP 探测工具自测"  tools/rtsp_probe_test.py
run "手柄输入层测试"     tools/gamepad_test.js
run "运动控制冒烟测试"   tools/smoke_test.sh
run "遥控服务端到端测试" tools/serve_test.sh
run "关停耗时测试"       tools/shutdown_test.sh

echo
if [[ $RC -eq 0 ]]; then
  echo "===== 全部测试通过 ====="
else
  echo "===== 有测试失败 ====="
fi
exit $RC
