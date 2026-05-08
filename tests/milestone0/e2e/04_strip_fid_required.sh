#!/usr/bin/env bash
# 配置请求了 MOVED_TO（需要 FAN_REPORT_FID）时，应当被剔除并继续启动
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built
need_fanotify_or_skip

WORK=$(mktemp -d /tmp/feb-e2e-XXXXXX)
SOCK="$WORK.sock"
LOG="$WORK.log"
PID=

cleanup() {
  stop_feb_bg "${PID:-}"
  rm -rf "$WORK" "$SOCK" "$LOG"
}
trap cleanup EXIT

# CLOSE_WRITE 默认开启；通过编辑 ad-hoc 的 TOML 引入 MOVED_TO，触发剔除告警
TOML="$WORK/cfg.toml"
cat > "$TOML" <<EOF
[monitor]
path = "$WORK"
events = ["CLOSE_WRITE", "MOVED_TO"]
EOF

PID=$(start_feb_bg "$LOG" -c "$TOML" -s "$SOCK" -l debug)
wait_for_socket "$SOCK"

# 服务正常起来 —— socket 已就绪，且日志包含剔除告警
assert_contains "Stripping events that require FAN_REPORT_FID" "$(cat "$LOG")"
