# Tests

FsEventBridge 的测试体系按里程碑组织，所有测试都既能在本地跑，也能在 GitHub Actions 中跑。

## Layout

```
tests/
├── lib/
│   ├── common.sh          # 共享 bash 工具：断言、后台进程、socket 等待
│   └── listener.py        # 端到端测试用的 UDS 监听器
├── fixtures/              # 各类 TOML 测试数据
├── milestoneN/
│   ├── unit/              # 不需要 root 的快速测试（依赖 --check-config）
│   └── e2e/               # 需要 fanotify 权限（root 或 setcap）
├── run.sh                 # 统一入口
└── README.md              # 本文件
```

每个里程碑独立一个目录（例如已有 `milestone0/`、`milestone1/`）。新增里程碑只需新建 `milestoneN/{unit,e2e}/`，并把对应的脚本文件丢进去；`run.sh` 自动发现。

## How to run

先确保已经 build：

```bash
bash scripts/build.sh
```

### 用 `tests/run.sh`（推荐，本地开发用）

```bash
bash tests/run.sh                              # 所有里程碑、所有类型
bash tests/run.sh --milestone 0                # 仅 milestone0
bash tests/run.sh --milestone 1 --type e2e     # NDJSON 语义（需 root/setcap）
bash tests/run.sh --milestone 0 --type unit    # 仅 unit
bash tests/run.sh --type e2e                   # 所有里程碑的 e2e（需要 root）
```

退出码：

- `0`  全部通过（含 SKIP）
- `1`  至少一个用例失败
- `77` 没有任何用例真正运行（全部 SKIP）

### 用 `ctest`（CI / 集成视角）

```bash
cd build
ctest --output-on-failure                      # 跑所有 add_test
ctest -L milestone0                            # 按 label 跑
ctest -L unit
ctest -L e2e
```

不可用的 e2e 套件会被 ctest 标记为 `Skipped`，不会影响整体结果。

### E2E 需要的权限

`fanotify_init` 默认只允许 `CAP_SYS_ADMIN` 调用。两种解决方式（任选其一）：

```bash
# 方式 1：sudo（每次都要密码 / NOPASSWD）
sudo -E bash tests/run.sh --type e2e

# 方式 2：给二进制打能力位（注意每次重新编译要重新设置）
sudo setcap cap_sys_admin+ep ./build/FsEventBridge
bash tests/run.sh --type e2e
```

在沙箱、受限容器、或缺乏 fanotify 支持的内核里，用例会**主动跳过**（exit 77），不会误报失败。

## 写一个新测试

### Unit 用例

放进 `tests/milestoneN/unit/NN_<name>.sh`：

```bash
#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built

OUT=$(feb_stdout -d /tmp/x --check-config)
assert_line "monitor_path=/tmp/x" "$OUT"
```

可用断言：`assert_line` / `assert_contains` / `assert_not_contains` / `assert_eq` / `assert_rc`。

### E2E 用例

放进 `tests/milestoneN/e2e/NN_<name>.sh`，必须先调用 `need_fanotify_or_skip`：

```bash
#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/../../lib/common.sh"
ensure_built
need_fanotify_or_skip

WORK=$(mktemp -d /tmp/feb-e2e-XXXXXX)
SOCK="$WORK.sock"
LOG="$WORK.log"
PID=

cleanup() { stop_feb_bg "${PID:-}"; rm -rf "$WORK" "$SOCK" "$LOG"; }
trap cleanup EXIT

PID=$(start_feb_bg "$LOG" -d "$WORK" -s "$SOCK" -l debug)
wait_for_socket "$SOCK"
# ... do work, assert ...
```

记得在脚本顶部 `chmod +x`，框架本身不强制，但 `bash <file>` 调用始终能跑。

## CI

- `.github/workflows/ci.yml`：每次 push / PR 自动跑（unit + e2e + ctest 摘要）。
- `.github/workflows/release.yml`：打 `v*` tag 时自动构建 `.deb` / `.rpm` 并附加到 Release。
