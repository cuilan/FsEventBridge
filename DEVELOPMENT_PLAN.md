# FsEventBridge 开发计划（未完成与优化项）

本文档用于把当前仓库中**已实现的 MVP**、**尚未实现但已预留的能力**、以及**可扩展/可优化方向**整理成可逐步推进的开发计划。后续建议你按“里程碑”逐个完成，每完成一个里程碑就补齐测试与文档，保证可回归、可验收。

## 背景与现状（MVP 已具备）

当前版本已跑通的核心链路：

- 监听：`fanotify`（`FAN_CLASS_NOTIF` + `FAN_MARK_FILESYSTEM`）捕获事件并通过 `/proc/self/fd/<fd>` 反解析路径
- 元数据：同步 `fstat` 获取 `size` 与 `mtime`（秒）
- 分发：Unix Domain Socket（UDS）广播 NDJSON（每行一条 JSON）
- 配置：默认值 + 可选 TOML + 命令行覆盖（两遍 `getopt_long`）
- 运维：支持 `sd_notify(READY=1/STOPPING=1)`，可用于 systemd 服务

## 重要说明（当前“文档/配置/CLI”与“实现”的不一致）

以下差异不一定是 bug，但会造成使用者误解，建议优先修正：

- **`recursive` 配置/参数未生效**：代码当前固定使用 `FAN_MARK_FILESYSTEM`，并未根据 `config.recursive` 调整标记策略。
- **`exclude_paths` 未生效**：配置结构体与 CLI 参数已支持收集，但监听循环中未对路径做过滤（目前只有扩展名过滤）。
- **`-i/--io-uring` 行为不完整**：帮助信息与 optstring 中包含 `-i`，但解析分支缺少 `case 'i'`；实际无法通过 CLI 切换该开关。
- **TOML 示例与解析键名不一致**：示例里出现 `exclude_path`，解析代码查找 `exclude_paths`；示例里的 `[processor]`、`[server].log_level` 也未被解析使用。
- **`io_uring` 仅初始化未参与热路径**：当前只做 `io_uring_queue_init/exit`，事件处理仍同步 `fstat`。
- **JSON 未输出事件类型**：`feb_event_t` 中计算了 `event_type`，但 NDJSON 仅输出 `mask`，下游只能自行解码。
- **`ts` 字段为 mtime 秒**：当前 `ts` 来源是 `st_mtim.tv_sec`，并非“事件发生的时间戳”，需要在文档中明确或调整实现。

## 里程碑路线图（建议顺序）

### Milestone 0：对齐配置、CLI 与行为（低风险，高收益） — 已完成

目标：让“看得见的配置/参数”确实能改变程序行为，减少误用成本。

完成项：

- `-i/--io-uring` 显式开启 + 新增 `--no-io-uring` 显式关闭。
- `monitor_loop` 中实现 `exclude_paths` 前缀匹配（带目录边界）。
- TOML 解析扩展：`[server].log_level`、`[processor].use_io_uring` 生效；
  排除路径键名兼容 `exclude_paths` / 旧版 `exclude_path`；空字符串自动剔除。
- `configs/config.toml` 与解析逻辑对齐，并标注当前已生效 / 暂未生效字段。
- 防御性：`build_event_mask` 对“需要 FAN_REPORT_FID 的事件位”自动剔除并告警，
  避免使用受限事件配置时 `fanotify_mark` 直接 `EINVAL`。
- 新增 `--check-config` 干跑标志：以非 root 身份打印解析后的最终配置后退出，
  使配置/参数解析具备可断言的稳定输出。

测试：

详见 [`tests/README.md`](tests/README.md)。可重复运行方式：

```bash
bash tests/run.sh --milestone 0 --type unit   # 不需要 root
sudo -E bash tests/run.sh --milestone 0 --type e2e
ctest --test-dir build -L milestone0          # 同样可用，CI 友好
```

CI / 发布：

- `.github/workflows/ci.yml`：每次 push / PR 自动 build + 跑 unit + 尝试 e2e（沙箱不可用时自动 SKIP）。
- `.github/workflows/release.yml`：打 `v*` tag 后自动构建 `.deb` / `.rpm` 并附加到 GitHub Release。

---

### Milestone 1：事件语义与输出模型增强（面向下游消费）

目标：下游不再需要依赖 `mask` 位运算来理解事件，输出更稳定、更容易演进。

待办：

- NDJSON 增加字段（保持向后兼容）：
  - `event`：字符串类型（例如 `"CLOSE_WRITE"`）
  - `type`：数值枚举（可选）
- 明确 `ts` 的语义：
  - 方案 A：保留 `mtime`，字段改名为 `mtime`，另增加真正的事件时间 `ts`
  - 方案 B：`ts` 改为采集时间（例如 `clock_gettime(CLOCK_REALTIME)` 秒/毫秒），`mtime` 另加字段
- 文档中提供 `mask` 常量对照与示例（尤其 `FAN_CLOSE_WRITE` 的 `0x8`）。
- 启用 fanotify FID 模式以支持目录变更类事件：
  - 背景：`FAN_CREATE` / `FAN_DELETE` / `FAN_MOVED_FROM` / `FAN_MOVED_TO` 等事件
    在内核里要求 `fanotify_init` 同时设置 `FAN_REPORT_FID`（或 `FAN_REPORT_DIR_FID`），
    否则 `fanotify_mark` 会以 `EINVAL` 失败。
  - 当前实现：`build_event_mask` 会主动剔除这些事件并打印告警，
    保证配置异常时不会让 `fanotify_mark` 失败，但也意味着这些事件“暂时无法上报”。
  - 计划：在 `monitor_init` 中开启 FID 模式，按 `fanotify_event_info_fid`
    解析事件，结合 `open_by_handle_at` 还原路径；同步在 NDJSON 中输出 `event` 字段。

验收标准：

- 客户端收到的 JSON 中包含 `event`，并且与 `mask` 语义一致。
- `ts/mtime` 字段在 README 与示例输出中可解释且一致。
- 配置 `events = ["CLOSE_WRITE","MOVED_TO","CREATE","DELETE"]` 后，
  程序能正常启动，且能在客户端看到对应的 `event` 字段。

建议测试：

- 使用 Python/Go 客户端对 `event` 字段做断言（创建/写入/关闭、移动、删除等）。

---

### Milestone 2：真正接入 io_uring（把“优化”落到热路径）

目标：把元数据读取或后续可能的 I/O 操作异步化，降低主循环阻塞与抖动。

推荐拆分（从易到难）：

- 阶段 2.1：仅将“可能阻塞的 stat/读取”迁移到工作队列（线程池），先保证结构正确与稳定。
- 阶段 2.2：在阶段 2.1 稳定后，再将工作队列替换为 `io_uring`（或将其作为可选后端）。

关键设计点（需要先定清楚）：

- fd 生命周期：`fanotify` 提供的 `metadata->fd` 必须在异步任务完成前保持有效；需要明确“何时 close”以及失败兜底。
- 背压策略：事件速率过高时，异步队列/环形队列的上限、丢弃策略、以及日志降噪策略。

验收标准：

- 开启 io_uring/异步后，事件主循环不因 `fstat` 等系统调用而长时间阻塞（可用简单压测对比）。
- 关闭 io_uring/异步后，仍能稳定回退到同步路径（行为一致）。

建议测试：

- 压测脚本：并发创建/写入/关闭大量小文件，比较 CPU、延迟、丢事件情况（先做相对指标即可）。

---

### Milestone 3：IPC 可靠性与可运维性（生产化）

目标：UDS 广播在慢客户端、多客户端、断连重连场景下表现可控，不会拖垮监控主循环。

待办：

- 处理 `send()` 的短写（partial write），避免 NDJSON 被截断。
- 客户端背压与隔离：
  - 方案 A：每客户端一个小缓冲队列，满则丢弃旧/新（可配置）
  - 方案 B：慢客户端直接踢下线（可配置）
- 连接管理改造：
  - 将 `accept` 从 `ipc_broadcast` 中拆出，加入 `poll/epoll`（可选），让“无事件也能接入客户端”行为更直觉。
- 可观测性：
  - 统计并输出关键指标：当前客户端数、丢弃事件数、发送失败次数、队列水位等（先日志，后续可 metrics）。

验收标准：

- 慢客户端不会导致服务整体阻塞或内存无界增长。
- 断连后 FD 能正确回收，客户端数统计不漂移。

建议测试：

- 人为制造“慢客户端”（读很慢或不读），观察服务端 CPU/内存与事件发送行为。

---

### Milestone 4：监控范围与权限模型完善（更贴近真实部署）

目标：把“监控范围”从当前的“文件系统级”扩展为更可控的策略，并把权限需求明确化。

待办方向：

- 明确并文档化当前模式：
  - `FAN_MARK_FILESYSTEM` 的语义与边界（为什么 `-r` 当前不改变范围）
  - 对 `CAP_SYS_ADMIN`/root 的依赖说明（WSL/VM/生产环境差异）
- 若需要“仅目录树”：
  - 评估 `FAN_MARK_MOUNT`/其他标记策略与实现复杂度
  - 明确递归策略：目录新增时是否需要额外 mark，如何处理移动/重命名

验收标准：

- README/使用指南明确告诉用户：当前需要什么权限、当前监控覆盖范围是什么、与 `-r` 的关系是什么。
-（可选）新增一种“目录树模式”时，提供清晰的模式选择参数与回归用例。

---

### Milestone 5：工程化补全（打包、服务、发布）

目标：让 deb/rpm/systemd 相关内容与运行时行为一致，方便安装部署与升级。

待办：

- 复核 `cpack` 打包脚本与 systemd service 文件：
  - 安装路径、默认配置路径、权限与用户、日志输出位置
  - postinst/prerm/postrm 脚本的幂等性与错误处理
- 增加发布说明：如何构建、如何安装、如何验证服务工作（含客户端示例）。

验收标准：

- 在干净环境安装 deb/rpm 后，服务可一键启动并产出可消费的 NDJSON。

## 风险清单与建议

- **范围过大导致“事件太吵”**：监控 `/tmp` 会出现大量 shell/编辑器临时文件事件，建议用专用目录或完善排除规则。
- **权限问题误判为程序问题**：`fanotify_init` 的 `EPERM` 常见于未具备能力的环境；文档与错误提示要足够明确。
- **行为一致性**：WSL2 可用于编译与功能验证，但最终仍以目标 Linux 环境（内核版本、文件系统、挂载方式）做性能与边界验证。

## 推荐的迭代节奏

建议先完成 `Milestone 0` 与 `Milestone 1`（对齐与输出模型），再做 `Milestone 3`（可靠性），最后再投入 `Milestone 2`（io_uring 热路径）。这样能先把“可用性与可维护性”做扎实，再做更重的性能工程。

