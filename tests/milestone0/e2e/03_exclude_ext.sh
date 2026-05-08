#!/usr/bin/env bash
# -e 指定的扩展名事件应被丢弃；其他扩展名仍正常上报
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built
need_fanotify_or_skip

WORK=$(mktemp -d /tmp/feb-e2e-XXXXXX)
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

PID=$(start_feb_bg "$LOG" -d "$WORK" -s "$SOCK" -e .swp -l debug)
wait_for_socket "$SOCK"

python3 "$LISTENER" --socket "$SOCK" --timeout 3 >"$OUT" 2>"$LERR" &
LPID=$!

sleep 0.3

for i in 1 2 3 4 5; do
  echo "swp-$i" > "$WORK/draft.swp"
  echo "txt-$i" > "$WORK/note.txt"
  sleep 0.2
done

wait "$LPID" || true

OUT_TXT=$(cat "$OUT")
if [ -z "$OUT_TXT" ]; then
  dump_e2e_diag "no events" "$OUT" "$LERR" "$LOG"
  fail "listener captured 0 events"
fi

assert_contains    "\"path\":\"$WORK/note.txt\"" "$OUT_TXT"
assert_not_contains "$WORK/draft.swp"            "$OUT_TXT"
