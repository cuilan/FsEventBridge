#!/usr/bin/env bash
# 验证 --help 输出包含关键参数与示例
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$("$FEB_BIN" --help 2>&1)
assert_contains "Usage:"            "$OUT"
assert_contains "feb [options]"     "$OUT"
assert_contains "fseventbridge"      "$OUT"
assert_contains "-c, --config"      "$OUT"
assert_contains "-d, --dir"         "$OUT"
assert_contains "-r, --recursive"   "$OUT"
assert_contains "-i, --io-uring"    "$OUT"
assert_contains "--no-io-uring"     "$OUT"
assert_contains "--check-config"    "$OUT"
assert_contains "-x, --exclude-path" "$OUT"
