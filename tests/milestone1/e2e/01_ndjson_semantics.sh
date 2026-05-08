#!/usr/bin/env bash
# Milestone 1：NDJSON 含 event/type/ts/mtime；ts 为墙钟秒，mtime 为文件 mtim 秒，CLOSE_WRITE 时 type/event 可读
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built
need_fanotify_or_skip

WORK=$(mktemp -d /tmp/feb-m1-XXXXXX)
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

PID=$(start_feb_bg "$LOG" -d "$WORK" -s "$SOCK" -l debug)
wait_for_socket "$SOCK"

python3 "$LISTENER" --socket "$SOCK" --timeout 5 >"$OUT" 2>"$LERR" &
LPID=$!

sleep 0.3
for i in 1 2 3 4 5; do
  echo "milestone1-$i" > "$WORK/m1.txt"
  sleep 0.2
done

wait "$LPID" || true

TXT=$(grep -F "\"path\":\"$WORK/m1.txt\"" "$OUT" || true)
if [ -z "$TXT" ]; then
  dump_e2e_diag "missing path line in listener output" "$OUT" "$LERR" "$LOG"
  fail "no NDJSON line for $WORK/m1.txt"
fi

assert_contains "\"event\":\"CLOSE_WRITE\"" "$TXT"
assert_contains "\"type\":1" "$TXT"
assert_contains "\"mask\":8" "$TXT"
assert_contains "\"ts\":" "$TXT"
assert_contains "\"mtime\":" "$TXT"
