#!/usr/bin/env bash
# 损坏的 TOML 必须以非零退出码失败
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

set +e
"$FEB_BIN" -c "$FIXTURES_DIR/invalid.toml" --check-config >/dev/null 2>&1
RC=$?
set -e

if [ "$RC" -eq 0 ]; then
  fail "expected non-zero exit for invalid TOML, got 0"
fi
