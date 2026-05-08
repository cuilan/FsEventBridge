# 变更说明

本项目版本号见仓库根目录 `VERSION`。以下按版本汇总面向使用方与开发者的重要变更。

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
