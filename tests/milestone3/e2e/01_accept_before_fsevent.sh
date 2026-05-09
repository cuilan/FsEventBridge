#!/usr/bin/env bash
# Milestone 3：在无 fanotify 事件时亦可 accept（poll 监听套接字与 fanotify 解耦）
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built
need_fanotify_or_skip

WORK=$(mktemp -d /tmp/feb-m3-XXXXXX)
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

PID=$(start_feb_bg "$LOG" -d "$WORK" -s "$SOCK" -l info)
wait_for_socket "$SOCK"

python3 "$LISTENER" --socket "$SOCK" --timeout 8 --count 1 >"$OUT" 2>"$LERR" &
LPID=$!

# 必须在产生任何文件事件前完成连接
sleep 0.4
wait_for_log "$LOG" "[IPC] New client connected"

echo "m3-accept-first" > "$WORK/x.txt"
wait "$LPID" || true

assert_contains "\"path\":\"$WORK/x.txt\"" "$(cat "$OUT")"
