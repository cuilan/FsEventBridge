#!/usr/bin/env bash
# Milestone 4：同文件系统上锚点目录外的路径不应上报（logical scope under --dir）
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built
need_fanotify_or_skip

WORK=$(mktemp -d /tmp/feb-m4-XXXXXX)
WATCHED="$WORK/watched"
SIBLING="$WORK/sibling"
mkdir -p "$WATCHED" "$SIBLING"
SOCK="$WORK.sock"
LOG="$WORK.log"
OUT="$WORK.out"
LERR="$WORK.lerr"
PID=

cleanup() {
  stop_feb_bg "${PID:-}"
  rm -rf "$WORK" "$SOCK" "$LOG" "$OUT" "$LERR"
}
trap cleanup EXIT

PID=$(start_feb_bg "$LOG" -d "$WATCHED" -s "$SOCK" -r -l debug)
wait_for_socket "$SOCK"

python3 "$LISTENER" --socket "$SOCK" --timeout 4 >"$OUT" 2>"$LERR" &
LPID=$!

sleep 0.3
for i in 1 2 3 4 5; do
  echo "in-$i"  > "$WATCHED/inside.txt"
  echo "out-$i" > "$SIBLING/outside.txt"
  sleep 0.2
done

wait "$LPID" || true

OUT_TXT=$(cat "$OUT")
if [ -z "$OUT_TXT" ]; then
  dump_e2e_diag "no events" "$OUT" "$LERR" "$LOG"
  fail "listener captured 0 events"
fi

assert_contains    "\"path\":\"$WATCHED/inside.txt\"" "$OUT_TXT"
assert_not_contains "\"path\":\"$SIBLING/outside.txt\"" "$OUT_TXT"
assert_not_contains "$SIBLING/outside.txt" "$OUT_TXT"
