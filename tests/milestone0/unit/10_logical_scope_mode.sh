#!/usr/bin/env bash
# --check-config：logical_scope 与 recursive=false 对齐为 direct_children
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$(feb_stdout -d /tmp/feb-scope --check-config)
assert_line "recursive=true"                "$OUT"
assert_line "logical_scope=subtree"         "$OUT"

OUT=$(feb_stdout -d /tmp/feb-scope --no-recursive --check-config)
assert_line "recursive=false"               "$OUT"
assert_line "logical_scope=direct_children" "$OUT"
assert_contains "direct_children=anchor path" "$OUT"
