#!/usr/bin/env bash
# CLI 应当覆盖 TOML 中的同名字段
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$(feb_stdout \
  -c "$FIXTURES_DIR/basic.toml" \
  -d /tmp/cli-override \
  -s /tmp/cli-feb.sock \
  -l error \
  --no-io-uring \
  -e .swp \
  -x /tmp/cli-override/skip \
  --check-config)

assert_line "monitor_path=/tmp/cli-override"         "$OUT"
assert_line "socket_path=/tmp/cli-feb.sock"          "$OUT"
assert_line "log_level=error"                        "$OUT"
assert_line "use_io_uring=false"                     "$OUT"
# CLI -e/-x 出现时会先清空配置文件来源的列表，仅保留 CLI 项
assert_line "exclude_extensions=.swp"                "$OUT"
assert_line "exclude_paths=/tmp/cli-override/skip"   "$OUT"
assert_line "ipc_per_client_queue_max=65536"         "$OUT"
assert_line "ipc_on_queue_full=skip_event"           "$OUT"
assert_line "logical_scope=subtree"                  "$OUT"
