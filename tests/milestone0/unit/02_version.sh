#!/usr/bin/env bash
# 验证 --version 输出版本号
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$("$FEB_BIN" --version 2>&1)
assert_contains "FsEventBridge Version:" "$OUT"
