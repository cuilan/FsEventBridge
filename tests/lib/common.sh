#!/usr/bin/env bash
# 共享测试工具：路径解析、断言、后台进程管理

set -euo pipefail

# 计算项目路径（无论从哪个测试脚本 source 都能定位）
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "$TESTS_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
FEB_BIN="${FEB_BIN:-$BUILD_DIR/FsEventBridge}"
FIXTURES_DIR="$TESTS_DIR/fixtures"
LISTENER="$TESTS_DIR/lib/listener.py"

# 终端颜色：仅在交互式 stdout 下启用，避免污染 CI 日志
if [ -t 1 ]; then
  C_GREEN='\033[0;32m'
  C_RED='\033[0;31m'
  C_YELLOW='\033[0;33m'
  C_CYAN='\033[0;36m'
  C_NC='\033[0m'
else
  C_GREEN=; C_RED=; C_YELLOW=; C_CYAN=; C_NC=
fi

# 跳过约定：与 ctest SKIP_RETURN_CODE 对齐
TEST_SKIP_CODE=77

ensure_built() {
  if [ ! -x "$FEB_BIN" ]; then
    echo -e "${C_RED}[FATAL]${C_NC} binary not found: $FEB_BIN"
    echo "Build it first:  bash scripts/build.sh"
    exit 1
  fi
}

fail() {
  echo -e "${C_RED}  FAIL:${C_NC} $*" >&2
  exit 1
}

skip() {
  echo -e "${C_YELLOW}  SKIP:${C_NC} $*" >&2
  exit "$TEST_SKIP_CODE"
}

# 断言两个字符串相等
assert_eq() {
  local expected="$1" actual="$2" desc="${3:-values}"
  if [ "$expected" != "$actual" ]; then
    fail "$desc expected='$expected' actual='$actual'"
  fi
}

# 断言整数等
assert_rc() {
  local expected="$1" actual="$2"
  if [ "$expected" -ne "$actual" ]; then
    fail "exit code expected=$expected actual=$actual"
  fi
}

# 断言 stdout 中存在某一整行（按整行精确匹配）
assert_line() {
  local needle="$1" haystack="$2"
  if ! printf '%s\n' "$haystack" | grep -Fxq -- "$needle"; then
    {
      echo "missing line: $needle"
      echo "--- output ---"
      printf '%s\n' "$haystack"
      echo "--------------"
    } >&2
    exit 1
  fi
}

# 断言 stdout 包含子串（不要求整行）
assert_contains() {
  local needle="$1" haystack="$2"
  if ! printf '%s\n' "$haystack" | grep -Fq -- "$needle"; then
    {
      echo "missing substring: $needle"
      echo "--- output ---"
      printf '%s\n' "$haystack"
      echo "--------------"
    } >&2
    exit 1
  fi
}

# 断言 stdout 不包含子串
assert_not_contains() {
  local needle="$1" haystack="$2"
  if printf '%s\n' "$haystack" | grep -Fq -- "$needle"; then
    {
      echo "unexpected substring: $needle"
      echo "--- output ---"
      printf '%s\n' "$haystack"
      echo "--------------"
    } >&2
    exit 1
  fi
}

# 仅捕获 feb 的 stdout，stderr 静默；返回真实退出码
feb_stdout() {
  local rc=0
  set +e
  "$FEB_BIN" "$@" 2>/dev/null
  rc=$?
  set -e
  return $rc
}

# ------------ E2E helpers ------------

# 真实探测：试启动一次 daemon，能拿到 socket 才认为环境可用
# 沙箱/容器即使是 root，fanotify_init 仍可能 EPERM，所以这里用主动探测最稳
need_fanotify_or_skip() {
  local probe_dir probe_sock probe_log probe_pid ok=0
  probe_dir=$(mktemp -d /tmp/feb-probe-XXXXXX)
  probe_sock="$probe_dir.sock"
  probe_log="$probe_dir.log"

  : > "$probe_log"
  "$FEB_BIN" -d "$probe_dir" -s "$probe_sock" -l error >>"$probe_log" 2>&1 &
  probe_pid=$!

  for _ in $(seq 1 30); do
    if [ -S "$probe_sock" ]; then ok=1; break; fi
    if ! kill -0 "$probe_pid" 2>/dev/null; then break; fi
    sleep 0.1
  done

  stop_feb_bg "$probe_pid"
  rm -rf "$probe_dir" "$probe_sock"

  if [ "$ok" -eq 1 ]; then
    rm -f "$probe_log"
    return 0
  fi

  if grep -q "fanotify_init failed" "$probe_log" 2>/dev/null \
     || grep -q "fanotify_mark failed" "$probe_log" 2>/dev/null; then
    rm -f "$probe_log"
    skip "fanotify not permitted (need CAP_SYS_ADMIN; check kernel/container constraints)"
  fi

  echo "--- probe log ---" >&2
  cat "$probe_log" >&2 || true
  echo "-----------------" >&2
  rm -f "$probe_log"
  fail "feb did not produce socket and did not raise a known fanotify error"
}

# 把 feb 起到后台，stdout/stderr 写入 logfile，回显 PID
# usage: pid=$(start_feb_bg <logfile> <args...>)
start_feb_bg() {
  local logf="$1"; shift
  : > "$logf"
  "$FEB_BIN" "$@" >>"$logf" 2>&1 &
  echo $!
}

# 优雅终止后台 feb；最多等 5s 再 KILL
stop_feb_bg() {
  local pid="${1:-}"
  [ -z "$pid" ] && return 0
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
      if ! kill -0 "$pid" 2>/dev/null; then
        return 0
      fi
      sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
  fi
}

# 等待 socket 文件出现
wait_for_socket() {
  local sock="$1" timeout="${2:-5}"
  local deadline=$(( $(date +%s) + timeout ))
  while [ ! -S "$sock" ]; do
    if [ "$(date +%s)" -gt "$deadline" ]; then
      fail "socket did not appear within ${timeout}s: $sock"
    fi
    sleep 0.05
  done
}

# e2e 失败时打印诊断信息：监听器 stdout / stderr / daemon 日志末尾
dump_e2e_diag() {
  local label="$1" out_file="$2" err_file="$3" log_file="$4"
  {
    echo "--- $label: listener stdout ---"
    if [ -f "$out_file" ]; then cat "$out_file"; else echo "(no $out_file)"; fi
    echo "--- $label: listener stderr ---"
    if [ -f "$err_file" ]; then cat "$err_file"; else echo "(no $err_file)"; fi
    echo "--- $label: daemon log (last 80 lines) ---"
    if [ -f "$log_file" ]; then tail -n 80 "$log_file"; else echo "(no $log_file)"; fi
    echo "------------------------------------------"
  } >&2
}

# 等待 logfile 中出现某子串（用于同步“客户端已连接”等事件）
wait_for_log() {
  local logf="$1" needle="$2" timeout="${3:-5}"
  local deadline=$(( $(date +%s) + timeout ))
  while ! grep -Fq -- "$needle" "$logf" 2>/dev/null; do
    if [ "$(date +%s)" -gt "$deadline" ]; then
      {
        echo "did not see in ${timeout}s: $needle"
        echo "--- log: $logf ---"
        cat "$logf" || true
        echo "------------------"
      } >&2
      exit 1
    fi
    sleep 0.05
  done
}
