#!/usr/bin/env bash
# FsEventBridge 测试入口
#
# 用法：
#   tests/run.sh                       # 跑所有里程碑的所有测试
#   tests/run.sh --milestone 0         # 仅跑 milestone0
#   tests/run.sh --type unit           # 跑所有里程碑的 unit
#   tests/run.sh --type e2e            # 跑所有里程碑的 e2e（需要 root/setcap）
#   tests/run.sh --milestone 0 --type unit
#
# 退出码：
#   0  全部通过（含 skip）
#   1  至少 1 个失败

set -uo pipefail

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TESTS_DIR/.." && pwd)"

if [ -t 1 ]; then
  C_GREEN='\033[0;32m'; C_RED='\033[0;31m'; C_YELLOW='\033[0;33m'
  C_CYAN='\033[0;36m';  C_BOLD='\033[1m';   C_NC='\033[0m'
else
  C_GREEN=; C_RED=; C_YELLOW=; C_CYAN=; C_BOLD=; C_NC=
fi

MILESTONE=""
TYPE="all"

while [ $# -gt 0 ]; do
  case "$1" in
    --milestone) MILESTONE="$2"; shift 2 ;;
    --type)      TYPE="$2";      shift 2 ;;
    -h|--help)
      sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# //; s/^#//'
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

case "$TYPE" in unit|e2e|all) ;; *)
  echo "invalid --type: $TYPE (expected unit|e2e|all)" >&2; exit 2 ;;
esac

# 收集要执行的目录列表
declare -a SUITES=()

discover() {
  local milestone_dir="$1"
  local milestone_name
  milestone_name=$(basename "$milestone_dir")
  if [ "$TYPE" = "all" ] || [ "$TYPE" = "unit" ]; then
    [ -d "$milestone_dir/unit" ] && SUITES+=("$milestone_name|unit|$milestone_dir/unit")
  fi
  if [ "$TYPE" = "all" ] || [ "$TYPE" = "e2e" ]; then
    [ -d "$milestone_dir/e2e" ] && SUITES+=("$milestone_name|e2e|$milestone_dir/e2e")
  fi
}

if [ -n "$MILESTONE" ]; then
  discover "$TESTS_DIR/milestone${MILESTONE}"
else
  for d in "$TESTS_DIR"/milestone*; do
    [ -d "$d" ] && discover "$d"
  done
fi

if [ "${#SUITES[@]}" -eq 0 ]; then
  echo "no test suite found"
  exit 0
fi

PASS=0; FAIL=0; SKIP=0
declare -a FAILED_TESTS=()

for entry in "${SUITES[@]}"; do
  IFS='|' read -r ms_name suite_type suite_dir <<< "$entry"
  echo
  echo -e "${C_BOLD}${C_CYAN}== $ms_name / $suite_type ==${C_NC}"

  shopt -s nullglob
  for t in "$suite_dir"/*.sh; do
    name=$(basename "$t" .sh)
    label="$ms_name/$suite_type/$name"
    bash "$t" >/tmp/_feb_test_out 2>&1
    rc=$?
    case "$rc" in
      0)  echo -e "  ${C_GREEN}PASS${C_NC}  $label"; PASS=$((PASS+1)) ;;
      77) echo -e "  ${C_YELLOW}SKIP${C_NC}  $label"; SKIP=$((SKIP+1)) ;;
      *)
        echo -e "  ${C_RED}FAIL${C_NC}  $label  (rc=$rc)"
        sed 's/^/    | /' /tmp/_feb_test_out
        FAIL=$((FAIL+1))
        FAILED_TESTS+=("$label")
        ;;
    esac
  done
  shopt -u nullglob
done

rm -f /tmp/_feb_test_out

echo
echo -e "${C_BOLD}Summary:${C_NC} ${C_GREEN}${PASS} passed${C_NC}, ${C_RED}${FAIL} failed${C_NC}, ${C_YELLOW}${SKIP} skipped${C_NC}"

if [ "$FAIL" -gt 0 ]; then
  echo "Failed tests:"
  for t in "${FAILED_TESTS[@]}"; do
    echo "  - $t"
  done
  exit 1
fi

# 没有任何用例真正执行（全部 SKIP）时返回 77，便于 ctest 标记为 Skipped
if [ "$PASS" -eq 0 ] && [ "$SKIP" -gt 0 ]; then
  exit 77
fi
exit 0
