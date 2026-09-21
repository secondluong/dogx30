#!/usr/bin/env bash
# 气体 3A 02 帧解析。需要先编译：
#   cmake -S rk3588 -B build && cmake --build build -j --target x30_gas_test

set -u
cd "$(dirname "$0")/.." || exit 1
BIN=build/x30_gas_test
if [[ ! -x $BIN ]]; then
  echo "找不到 $BIN，请先编译。"
  exit 1
fi
exec "$BIN"
