#!/usr/bin/env bash
# --no-io-uring 必须能关掉默认开启的 io_uring
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

# 默认开启
OUT=$(feb_stdout -d /tmp/x --check-config)
assert_line "use_io_uring=true"  "$OUT"

# --no-io-uring 关闭
OUT=$(feb_stdout -d /tmp/x --no-io-uring --check-config)
assert_line "use_io_uring=false" "$OUT"

# -i 显式开启可覆盖配置文件中的 false
OUT=$(feb_stdout -c "$FIXTURES_DIR/basic.toml" -i --check-config)
assert_line "use_io_uring=true"  "$OUT"
