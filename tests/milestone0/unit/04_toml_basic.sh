#!/usr/bin/env bash
# 加载基本 TOML 时各字段被正确解析
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$(feb_stdout -c "$FIXTURES_DIR/basic.toml" --check-config)

assert_line "monitor_path=/tmp/febtest"          "$OUT"
assert_line "socket_path=/tmp/feb-test.sock"     "$OUT"
assert_line "recursive=true"                     "$OUT"
assert_line "use_io_uring=false"                 "$OUT"
assert_line "log_level=warn"                     "$OUT"
# 0x8 (CLOSE_WRITE) | 0x2 (MODIFY) = 0xa
assert_line "event_mask=0xa"                     "$OUT"
# events 行包含两个事件，顺序由 print_event_names 决定
assert_contains "CLOSE_WRITE"                    "$OUT"
assert_contains "MODIFY"                         "$OUT"
assert_line "exclude_extensions=.bak"            "$OUT"
assert_line "exclude_paths=/tmp/febtest/skip"    "$OUT"
