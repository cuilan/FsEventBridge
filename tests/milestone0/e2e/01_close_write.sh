#!/usr/bin/env bash
# CLOSE_WRITE 路径上的端到端：写入并关闭文件后，客户端应收到对应事件
#
# 设计要点：
# - 不依赖随机 /tmp 事件来同步；监听器启动后给 connect() 一点时间，
#   随后我们循环生成事件，第一条命中“listener 已连接”后就会触发 accept + send。
# - 监听器不限定 count，按时间窗口收满后退出，然后用 grep 过滤我们关心的路径。
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

PID=$(start_feb_bg "$LOG" -d "$WORK" -s "$SOCK" -l debug)
wait_for_socket "$SOCK"

python3 "$LISTENER" --socket "$SOCK" --timeout 3 >"$OUT" 2>"$LERR" &
LPID=$!

# 给 listener 的 connect() 一点时间，随后多次触发事件以保证至少有一次命中已连接窗口
sleep 0.3
for i in 1 2 3 4 5; do
  echo "trigger-$i" > "$WORK/hello.txt"
  sleep 0.2
done

wait "$LPID" || true

OUT_TXT=$(cat "$OUT")
if [ -z "$OUT_TXT" ]; then
  dump_e2e_diag "no events" "$OUT" "$LERR" "$LOG"
  fail "listener captured 0 events"
fi

assert_contains "\"path\":\"$WORK/hello.txt\"" "$OUT_TXT"
