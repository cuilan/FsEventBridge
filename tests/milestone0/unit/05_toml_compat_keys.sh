#!/usr/bin/env bash
# 兼容旧键名 exclude_path（单数）；空字符串应被自动剔除
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$(feb_stdout -c "$FIXTURES_DIR/compat_keys.toml" --check-config)

# 旧键名 exclude_path 的两个非空项被加载、空字符串被丢弃
assert_line "exclude_paths=/tmp/febtest/cache,/tmp/febtest/legacy" "$OUT"
