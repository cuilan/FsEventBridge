# 变更说明

本项目版本号见仓库根目录 `VERSION`。以下按版本汇总面向使用方与开发者的重要变更。

## [1.4.0] — Milestone 3 后续：IPC 可调与 POLLOUT

- **`[ipc]` 配置**：`per_client_queue_max_bytes`（0 或未写则用内置默认 256 KiB）、`on_queue_full`（`disconnect` / `discard_pending` / `skip_event`）。
- **`poll`** 对每个有积压的客户端追加 **POLLOUT**，可写时再 **flush**，与循环末尾 **`ipc_idle_flush`** 互补。
- **`--check-config`** 打印 `ipc_per_client_queue_max`（最终生效值）与 `ipc_on_queue_full`。
- **`ipc_init`** 接收 **`feb_config_t *`**（可传快照以应用上述选项）。

---

## [1.3.0] — Milestone 3：IPC 可靠性与可运维性

- **非阻塞客户端套接字**；`send` 处理**短写**，未完成数据进入**每连接内存队列**（默认上限 **256 KiB**/连接），超限则**断开**该客户端，避免全局阻塞与内存无界增长。
- **`ipc_broadcast` 不再内嵌 `accept`**：`monitor_loop` 使用 **`poll(fanotify, listen_fd)`**，在无 fanotify 事件时亦可 **`accept`** 新连接；每轮循环末尾 **`ipc_idle_flush`** 尝试刷出积压。
- **可观测性**：约每 **2000** 次广播输出一条 **`[IPC] stats`**（clients、发送字节、各类断开计数）。
- **测试**：`tests/milestone3/e2e/01_accept_before_fsevent.sh`；CMake **`milestone3_e2e`**。

---

## [1.2.0] — Milestone 1：NDJSON 语义与扩展

### NDJSON（不保证与 1.1.x 字段兼容）

- 新增可读字段 **`event`**（如 `CLOSE_WRITE`）、整型 **`type`**（与枚举一致）。
- **`ts`**：网关处理事件并完成 `fstat` **之后**的墙钟时刻（**`CLOCK_REALTIME`**，Unix **秒**）。
- **`mtime`**：文件 **`st_mtim`** 秒；**`fstat` 失败时为 `-1`**。
- 保留 **`path`、`size`、`mask`**。
- **`event_type` 枚举**以 **`UNKNOWN = 0`** 为首个取值，其后为各类 fanotify 事件类型。

### 实现与运维

- 抽取 **`feb_event_name()`**，供 NDJSON 与日志共用同一套名称字符串。
- **DEBUG** 级别下 `Event sent` 日志输出 **`event`、`type`、`ts`、`mtime`、`mask`、`size`**，便于不连客户端时对账。
- CMake **`ctest`** 增加 **`milestone1_e2e`**；脚本见 `tests/milestone1/`。

### 文档与计划

- **README**：特性表述与实现对齐（C17、NFS 客户端挂载与内核边界、性能/io_uring 演进说明）；补充 NDJSON 字段表。
- **DEVELOPMENT_PLAN.md**：Milestone 1 标为已完成；增补 **NFS 兼容性矩阵**与**性能回归**的建议章节。

### 工程

- **`VERSION`**：1.2.0。
- **GitHub Actions**：`checkout` / `upload-artifact` 等升级与 Node 24 相关工作流变量（若有），减轻弃用告警。

---

## [1.1.0] — Milestone 0：配置对齐、回归测试与 CI

- CLI/TOML：`exclude_paths`、`-i` / `--no-io-uring`、`--check-config`、`[server].log_level`、兼容 `exclude_path`；`fanotify_mark` 前剔除需 FID 的事件位。
- 测试套件 `tests/`、`ctest`、CI/Release workflow。

（更早版本可从 git 历史检索。）
