#!/usr/bin/env bash
# -x 指定的前缀目录及其子树事件应被丢弃；其他路径仍正常上报
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built
need_fanotify_or_skip

WORK=$(mktemp -d /tmp/feb-e2e-XXXXXX)
mkdir -p "$WORK/keep" "$WORK/skip"
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

PID=$(start_feb_bg "$LOG" -d "$WORK" -s "$SOCK" -x "$WORK/skip" -l debug)
wait_for_socket "$SOCK"

python3 "$LISTENER" --socket "$SOCK" --timeout 3 >"$OUT" 2>"$LERR" &
LPID=$!

sleep 0.3

# 同时往“被排除”和“正常”两个目录写入；多次以避免单次错过 accept 的时机
for i in 1 2 3 4 5; do
  echo "skip-$i" > "$WORK/skip/dropped.txt"
  echo "keep-$i" > "$WORK/keep/seen.txt"
  sleep 0.2
done

wait "$LPID" || true

OUT_TXT=$(cat "$OUT")
if [ -z "$OUT_TXT" ]; then
  dump_e2e_diag "no events" "$OUT" "$LERR" "$LOG"
  fail "listener captured 0 events"
fi

assert_contains    "\"path\":\"$WORK/keep/seen.txt\""  "$OUT_TXT"
assert_not_contains "$WORK/skip/dropped.txt"           "$OUT_TXT"
