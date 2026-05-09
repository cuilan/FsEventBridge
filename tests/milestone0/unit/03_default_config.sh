#!/usr/bin/env bash
# 不提供配置文件时的默认值
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$(feb_stdout -d /tmp/febtest --check-config)

assert_line "monitor_path=/tmp/febtest"   "$OUT"
assert_line "socket_path=/tmp/feb.sock"   "$OUT"
assert_line "recursive=true"              "$OUT"
assert_line "logical_scope=subtree"       "$OUT"
assert_contains "FAN_MARK_FILESYSTEM"    "$OUT"
assert_line "use_io_uring=true"           "$OUT"
assert_line "log_level=info"              "$OUT"
assert_line "event_mask=0x8"              "$OUT"
assert_line "events=CLOSE_WRITE"          "$OUT"
assert_line "exclude_extensions="         "$OUT"
assert_line "exclude_paths="              "$OUT"
assert_line "ipc_per_client_queue_max=262144" "$OUT"
assert_line "ipc_on_queue_full=disconnect"    "$OUT"
