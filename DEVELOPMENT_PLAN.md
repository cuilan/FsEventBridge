# FsEventBridge 开发计划（完成度评估、路线图与打包策略）

本文档汇总 **已实现能力**、**与里程碑的对应关系**、**尚需推进的工作（含打包与运维）**，以及建议的优先级。建议你按里程碑与「发行渠道」分列迭代：功能里程碑走 `tests/` + `ctest`；正式进入发行版存档（Debian / Fedora 等）时走各自社区的打包规范。

---

## 完成度总览（截至 VERSION 文件当前版本）

| 维度 | 状态 | 说明 |
|------|------|------|
| 核心链路（fanotify → 元数据 → NDJSON → UDS） | **已具备** | MVP 级生产可用度，依赖 `CAP_SYS_ADMIN` / root 与内核行为 |
| 配置与 CLI 对齐 | **大部分完成** | Milestone 0：TOML/CLI、`exclude_paths`、`--check-config`、io_uring 开关等已落地 |
| 事件语义与 NDJSON | **已完成** | Milestone 1：`event`/`type`/`ts`/`mtime` 等字段与文档对齐 |
| IPC 可靠性与背压 | **已完成** | Milestone 3：非阻塞、`poll`、每连接队列、策略可配 |
| io_uring 深度用于热路径 | **未启动** | 仍为 Milestone 2：库初始化与配置存在，异步化未入主循环 |
| 监控语义与 `-r`/挂载策略 | **已完成（Milestone 4）** | 逻辑锚点过滤 + `recursive` / `--no-recursive`；仍为 `FAN_MARK_FILESYSTEM`；可选 `FAN_MARK_MOUNT` 等见下文 backlog |
| 上游一键包（CMake CPack） | **基础具备** | 可产出 `.deb` / `.rpm`；CI Release 流程已挂载 |
| **符合 Debian 政策的源码包（`debian/`）** | **未具备** | 当前为 CPack「二进制分包」，未达到官方存档 / 衍生版常用的 `dpkg-buildpackage` 规范 |
| **符合 Fedora/RHEL 习惯的 SPEC + mock** | **未具备** | 当前 CPack RPM 与 Debian 同源 `postinst` 语义需复核，不推荐直接宣称「等价于发行版 RPM 规范」 |

**结论**：作为 **GitHub 首版 / 自托管部署**，当前功能与 CPack 产物已可验证；若目标包括 **进入 Debian、Ubuntu 官方或严肃企业基线**，需规划 **独立打包轨道**（见下文「发行与打包策略」），与功能里程碑并行。

---

## 背景与现状（核心能力简述）

- 监听：`fanotify`（`FAN_CLASS_NOTIF` + `FAN_MARK_FILESYSTEM`）与路径反查；用户态 **逻辑锚点过滤**（见 Milestone 4）后再分发
- 元数据：同步 `fstat`（`size`、`mtime`）
- 分发：UDS 广播 NDJSON
- 配置：默认 + TOML + 命令行覆盖；`--check-config` 干跑
- 运维：`sd_notify`；systemd 单元 **`fseventbridge.service`**；安装路径 **`/etc/fseventbridge/`**

---

## 实现与对外承诺之间仍须注意的差异（精简版）

以下用于 **避免过度承诺**；此处只保留 **仍成立或部分成立** 的条目：

1. **`io_uring` 热路径**：`liburing` 仍在工程内，但 **事件处理路径未异步化**；性能优化归属 Milestone 2。
2. **需 `FAN_REPORT_FID` 的事件位**：为规避 `fanotify_mark` 在部分配置下的 `EINVAL`，构建事件掩码时仍会 **剔除** 依赖 FID 的位；完整 FID 模式属后续能力。
3. **打包维护脚本**：Debian 的 `postinst`/`prerm`/`postrm` 与 **RPM 脚本参数语义不同**；**CPack** 生成 RPM 时若复用 Debian 同源脚本需谨慎（见 Milestone 5）。

---

## 里程碑路线图与当前状态

### Milestone 0：对齐配置、CLI 与行为 — **已完成**

要点（摘要）：

- **CLI**：`-i/--io-uring`、`--no-io-uring`。
- **过滤**：`monitor_loop` 内 **`exclude_paths`** 前缀匹配（含目录边界）。
- **TOML**：`[server].log_level`、`[processor].use_io_uring`；**`exclude_paths` / `exclude_path`** 键名兼容；空字符串自动剔除。
- **示例配置**：`configs/config.toml` 与解析逻辑对齐。
- **防御**：**`build_event_mask`** 剔除需 `FAN_REPORT_FID` 等会导致 `fanotify_mark` 失败的事件位。
- **干跑**：**`--check-config`** 可在非 root 下打印解析后的最终配置（便于测试与自动化）。

测试与 CI：见 [`tests/README.md`](tests/README.md)；`.github/workflows/ci.yml`。

**补充（v1.5.0 起）**：安装名与系统路径 **Debian 风格小写**（`fseventbridge`、`feb`、`/etc/fseventbridge`、`fseventbridge.service`）；文档中可保留 **FsEventBridge** 作为项目称谓。

重复运行示例：

```bash
bash tests/run.sh --milestone 0 --type unit
sudo -E bash tests/run.sh --milestone 0 --type e2e
ctest --test-dir build -L milestone0
```

---

### Milestone 1：事件语义与输出模型 — **已完成**

- NDJSON：`path`、`event`（可读名）、`type`（整型枚举）、`size`、`ts`、`mtime`、`mask`。
- **语义**：**`ts`** = 网关处理并完成 `fstat` 后的 **CLOCK_REALTIME** 纪元秒；**`mtime`** = `st_mtim` 秒（失败时为 `-1`）。
- 完整 **FID/`FAN_REPORT_FID`** 语义与掩码仍属后续扩展。

测试：`tests/milestone1/e2e/`，`ctest -L milestone1`。

---

### Milestone 2：真正接入 io_uring（热路径优化） — **未开始**

建议仍按 **阶段 2.1（线程池/workqueue）→ 2.2（io_uring 后端）** 递进；事先明确 **fanotify 返回 fd 的生命周期**（异步完成前不得 `close`）、队列上限与背压/日志降噪策略。

验收方向：开启异步路径后主循环不因 **`fstat`** 等长时间阻塞；关闭时回退至同步路径行为一致。建议增加 **压测脚本**（大量小文件创建/写入/关闭），以 **相对指标** 比较即可。

**优化项**：在 Milestone 2 前后可增加 **粗粒度基准脚本**（例如 `scripts/bench-*.sh`），结果在本机归档；**CI 不作为绝对性能数据源**，仅做功能回归。

---

### Milestone 3：IPC 可靠性与可运维性 — **已完成**

要点：非阻塞 `send`；每连接待发送队列（可配置上限与满队列策略）；**`poll`** 同时监听 fanotify 与 UDS listen，`accept` 与广播解耦；循环末尾 **`ipc_idle_flush`**；约每 2000 次广播输出 **`[IPC] stats`** 类英文日志。

测试：`tests/milestone3/e2e/`，`ctest -L milestone3`。

---

### Milestone 4：监控范围与权限模型 — **已完成**

交付内容：

- **实现**：`monitor.c` 中 **`event_path_in_scope`**：在 **`exclude_*` 之前**，仅保留 **`--dir` / `[monitor].path` 锚点** 之下的路径事件；同文件系统上锚点之外的兄弟挂载路径不再转发 NDJSON。**`recursive=false`**（及 CLI **`--no-recursive`**）时仅锚点自身与**直接进入的一层**路径。
- **CLI**：新增 **`--no-recursive`**，与 **`--no-io-uring`** 风格一致。
- **可观测**：**`monitor_init`** 成功后一条 **英文 INFO**（`fanotify: FAN_MARK_FILESYSTEM ... forwarding only logical scope ...`）。
- **文档**：[`README.md`](README.md) 增加「运行权限」「监控范围与 recursive」小节；[`configs/config.toml`](configs/config.toml) 注释；说明 **WSL/容器/`EPERM`** 的常见差异。
- **测试**：[`tests/milestone4/e2e/`](tests/milestone4/e2e/)（子树与同盘兄弟目录、e2e 非递归语义）；**`ctest -L milestone4`**。

可选 backlog（不占本里程碑验收）：

- **仅挂载级事件聚合策略**（例如评估 **`FAN_MARK_MOUNT`**）：与锚点选型、降噪、跨子卷行为相关；需要单独设计与压测。

---

### Milestone 5：发行、打包与多端维护脚本 — **部分完成 · 拆分子目标**

现状：

- CMake **`install`**：二进制、`feb` 符号链接、`/etc/fseventbridge/config.toml`、`/lib/systemd/system/fseventbridge.service`。
- **CPack**：`DEB` + `RPM` 生成器；`scripts/cpack.sh` 封装；Release 工作流 **`cpack` + artifact + gh-release**。
- **维护脚本**：`package/postinst`、`prerm`、`postrm`（偏 Debian）；`CMakeLists.txt` 中 RPM 使用了 **与 Debian 同源**的 `postinst` — **RPM 场景下存在脚本参数不匹配的隐患**。

建议将 Milestone 5 拆分为可独立验收的子项：

#### 5.1 上游 CPack 兜底（短期，低成本）

- **Debian**：核对 **`CPACK_DEBIAN_PACKAGE_DEPENDS`** 与目标发行版的 **`libsystemd*` / `liburing*`** 包名是否在 **Debian Stable/Testing** 上一致（不同发行版 SONAME 与包名会变）。
- **RPM**：增加 **`CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE`** 等对卸载链路的脚本占位；**将 systemd 编排从「Debian-only postinst」中拆出**，改为：
  - 要么 **RPM 专用** `%post`/`%preun` 脚本文件，或  
  -  **`package/maintainer.sh`** 内含对 `DEBIAN_SCRIPT` / `RPM_INSTALL` 的检测分支（需注意可维护性）。
- **Lint 与元数据**：把 `CPACK_PACKAGE_CONTACT`、`CPACK_DEBIAN_PACKAGE_MAINTAINER`、Vendor 从占位字符串改为仓库真实联系方式；按需补充 **`LICENSE`** 安装与 **`copyright`** 条目（便于下游打包复用）。
- **验收**：在 **Ubuntu 24.04** 与至少一个 **RPM 目标**（如 `fedora:40` 容器）内：**安装 → `systemctl enable --now` → 冒烟**，并验证卸载不残留错误失败状态。

#### 5.2 符合 **Debian 政策**的源码包（中期，独立于 CPack）

**目标**：可运行 **`dpkg-buildpackage`**（或 **`debuild`**），产出 **`.dsc` + `orig.tar.*` + `debian/`**，满足衍生版或官方 **NEW** 流程的常见要求（非「仅一个二进制 .deb」）。

建议内容（与工作树平行，例如在 `dist/debian/` 或顶层 `debian/`，按需选择是否与主分支同仓维护）：

- **`debian/rules`**：`dh`，`DEB_BUILD_HARDENING`、`*FLAGS`。
- **`debian/control`**：`Build-Depends`（cmake、pkg-config、`libsystemd-dev`、`liburing-dev` 等）；`Depends`/`Recommends`；**二进制包名 **`fseventbridge`** 与 CMake 一致。
- **`debian/changelog`**：维护者、发行版代号、条目与上游 **VERSION / CHANGELOG** 同步流程。
- **`debian/copyright`**：**dep5**；与仓库许可证一致。
- **`debian/*.install`**、**`debian/fseventbridge.service`**（可符号链接到 `package/` 下同名文件减少重复）。
- **`debian/source/format`**（`3.0 (quilt)` 等）。
- **Lintian**：本地或 CI 跑 **`lintian`**，对 **E/W** 分级消减。

**与 CPack 关系**：Debian 官方通常 **不接受** 仅由 CPack 生成的 tarball 作为唯一来源；**源码包轨道** 是「进存档」路径，**CPack** 适合 **GitHub Release 附资产** — 二者可并存。

#### 5.3 符合 **RPM / Fedora** 习惯的打包（中期）

**目标**：提供 **`fseventbridge.spec`**（或 `packaging/rpm/`），可在 **Fedora mock** / **rpmbuild -ba** 下完整构建，并符合 **Fedora Packaging Guidelines** 的常见条项（非 CPack 一键替代）。

建议内容：

- **`%prep` / `%build` / `%install`**：调用 CMake，使用发行版 **`%cmake`** 宏或显式 **`-DCMAKE_INSTALL_PREFIX=/usr`**。
- **脚本段**：**`%post`** / **`%preun`** 使用 **`systemd_post`** / **`systemd_preun`** 宏（发行版提供），或手写与 **RPM 参数约定**一致的 `systemctl` 调用。
- **文件列表**：**`%files`** 显式列出 **`%license`**、二进制、`/usr/lib/systemd/system/fseventbridge.service`、**`%config(noreplace) /etc/fseventbridge/config.toml`** 等。
- **依赖**：`BuildRequires` / `Requires` 使用发行版包名（`systemd-devel`、`liburing-devel` 等）。

**验收**：在 **Fedora 或 RHEL+CRB/EPEL 构建环境** 中完成 **SRPM → 二进制 RPM**，并做一次 **install + systemd** 冒烟。

#### 5.4 发布与物料（与 5.1 联动）

- **SBOM / 签名**（按需）：deb **`.buildinfo`**、RPM **GPG 签章**。
- **Release Checklist**：tag → CI 产物 → **校验和** → GitHub Release 说明中引用 **CHANGELOG** 与 **升级注意**（尤其 **1.5.0** 路径与单元名变更）。

---

## （建议）兼容性矩阵：本地 FS 与 NFS 客户端挂载

**背景**：FsEventBridge（安装命令 **`fseventbridge`**）不向 NFS 服务端打补丁；仅在本机对已挂载路径使用 **`fanotify`**。事件是否完整取决于 **客户端内核是否沿 VFS 路径派发 fanotify**。

**里程碑目标**：文档化「已实测」组合；可选 `tests/` 或 `scripts/` 中依赖 **人工准备 NFS** 的冒烟流程。**与 Milestone 4** 一并规划更合适。

---

## （建议）性能测试与回归

在 Milestone 2 定型后做系统压测更有意义；此前可用轻量脚本防明显退化。

| 维度 | 说明 |
|------|------|
| 吞吐 | listener 收到的 events/sec |
| CPU / 内存 | `time`、`/proc` 或 `perf stat` |
| 延迟（可选） | 事件到 NDJSON 的粗分布 |

**CI**：托管 Runner **不作绝对性能基线**；真机归档基线更合适。

---

## 风险清单与建议

- **监控范围过大**：建议专用目录与排除规则。
- **`EPERM` 与权限**：文档与报错信息需明确 **非程序缺陷**的情形。
- **环境差异**：最终以目标 **内核 / 文件系统 / 挂载** 为准验证；WSL2 多用于开发自测。
- **双打包体系认知**：**CPack = 上游便捷产物**；**Debian/`debian/` 与 Fedora `.spec` = 进社区存档的正规路径**，维护成本更高但可预期。

---

## 推荐的迭代节奏

1. **短期**：收口 **Milestone 5.1**（RPM 脚本与依赖名、占位 Maintainer、卸载链、容器内安装冒烟）。
2. **中期**：立项 **Milestone 2**，并行孵化 **`debian/`** 或 **`*.spec`** 之一（取决于你优先打入的生态）。
3. **长期**：FID 扩展、NFS 矩阵、性能基线与（可选）**SELinux**、**debconf**/**firewalld** 等企业化需求单列评估。
