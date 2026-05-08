#!/usr/bin/env bash
# events = [] 应当回退到默认 CLOSE_WRITE，避免 fanotify_mark 失败
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$(feb_stdout -c "$FIXTURES_DIR/empty_events.toml" --check-config)

assert_line "event_mask=0x8"     "$OUT"
assert_line "events=CLOSE_WRITE" "$OUT"
