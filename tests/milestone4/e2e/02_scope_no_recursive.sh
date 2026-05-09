#!/usr/bin/env bash
# Milestone 4：--no-recursive 时锚点目录的深层路径不应上报，仅锚点直下路径可上报
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built
need_fanotify_or_skip

WORK=$(mktemp -d /tmp/feb-m4-XXXXXX)
mkdir -p "$WORK/sub/deep"
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

PID=$(start_feb_bg "$LOG" -d "$WORK" -s "$SOCK" --no-recursive -l debug)
wait_for_socket "$SOCK"

python3 "$LISTENER" --socket "$SOCK" --timeout 5 >"$OUT" 2>"$LERR" &
LPID=$!

sleep 0.3
for i in 1 2 3 4 5; do
  echo "direct-$i" > "$WORK/shallow.txt"
  echo "deep-$i"    > "$WORK/sub/deep/nested.txt"
  sleep 0.2
done

wait "$LPID" || true

OUT_TXT=$(cat "$OUT")
if [ -z "$OUT_TXT" ]; then
  dump_e2e_diag "no events" "$OUT" "$LERR" "$LOG"
  fail "listener captured 0 events"
fi

assert_contains    "\"path\":\"$WORK/shallow.txt\"" "$OUT_TXT"
assert_not_contains "\"path\":\"$WORK/sub/deep/nested.txt\"" "$OUT_TXT"
