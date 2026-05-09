# FsEventBridge (FEB)

FsEventBridge 是一款基于 Linux 内核 `fanotify`、`io_uring`（可选）等能力的文件系统事件网关，将内核文件事件转为 **NDJSON** 流，经 **Unix Domain Socket** 分发给 Go、Python、Java 等上游。

---

## 🚀 核心特性

* **内核级大范围监控**：基于 `fanotify` 在**锚点路径所在的文件系统/挂载**上打标，避免 `inotify` 那样对每个子目录逐个 `add_watch`；实际覆盖范围取决于内核与挂载类型（见下文 NFS 说明）。
* **NFS 客户端挂载**：**无需改动 NFS 服务端**；在**本机 NFS 客户端挂载点**上，事件是否出现、是否齐全，由 **Linux 内核 + NFS 客户端实现（及 NFS 版本）**共同决定。**本仓库当前没有单独的「NFS 专用协议」代码路径**——与同机 ext4/XFS 等相比，**不能保证**事件语义与覆盖率完全一致，需要在目标内核上实测（详见 `DEVELOPMENT_PLAN.md` 中的兼容性验证）。
* **C17 + 严控告警**：源码按 **ISO C17**（`CMake` 设定 `C_STANDARD 17`，并启用 `-Wall -Wextra -Werror`，另使用 `_GNU_SOURCE` 以获得 Linux 必需的 POSIX/GNU API）。
* **高性能取向**：链路以 **`fanotify` + 精简 NDJSON 推送**为主；已链接 **`liburing`** 并支持初始化，**异步 I/O 在热路径上的落地仍在演进**（见路线图 Milestone 2），适合作为后续压测与优化的基线。
* **跨语言集成**：通过 Unix Domain Socket 发送 **NDJSON（每行一条 JSON）**，便于多语言消费。
* **工业级部署**：集成 **Systemd** 就绪通知，提供 **`.deb` / `.rpm`** 打包路径，贴合常见 Linux 运维方式。
* **IPC 可靠性（v1.3+）**：客户端为非阻塞发送，短写自动排队；单连接队列上限与队列满策略可由 **`[ipc]`** 配置（默认约 256 KiB、超限时断开）。`poll` 监听 fanotify、监听套接字及对慢客户端的 **POLLOUT**，可在无磁盘事件时接入下游并排空积压。运维日志每约 2000 次广播输出一次 **`[IPC] stats …`**。

---

## 🛠 安装与构建

### 依赖

* Linux Kernel >= 5.1（推荐 6.x）
* GCC >= 12（支持 C17）
* CMake、pkg-config
* liburing、libsystemd（开发时需对应 `-dev` 包）

### 编译（及可选打包）

也可使用仓库内脚本：`bash scripts/build.sh`

```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j"$(nproc)"

# 可选：生成 Debian / RPM 安装包（需本机装有 cpack 相应生成器）
cpack
```

---

## 📖 使用指南

### 运行权限（必读）

监听文件事件需要 **`CAP_SYS_ADMIN`**。任选其一：

* 临时：`sudo ./FsEventBridge …`
* 开发便利（每次重新编译后需重设）：`sudo setcap cap_sys_admin+ep ./build/FsEventBridge`，之后可直接以普通用户运行。

### 命令行示例

监控目录、指定 Socket，并打印调试日志：

```bash
sudo ./FsEventBridge -d /data/sate -s /tmp/feb.sock -l debug -r
```

常用选项（完整列表见 `./FsEventBridge --help`）：

| 选项 | 作用 |
|------|------|
| `-d, --dir` | 监控路径 |
| `-s, --socket` | UDS 路径（默认 `/tmp/feb.sock`） |
| `-c, --config` | TOML 配置文件 |
| `-r, --recursive` | 递归相关配置项（与 fanotify 行为配合，详见帮助） |
| `-l, --log-level` | `debug` / `info` / `warn` / `error` |
| `-e, --exclude-ext` | 按扩展名排除（可重复） |
| `-x, --exclude-path` | 按路径前缀排除（可重复） |
| `-i, --io-uring` | 启用 io_uring 相关初始化 |
| `--no-io-uring` | 关闭上述开关 |
| `--check-config` | 加载配置后打印最终生效项并退出（无需 root） |
| `-v, --version` | 版本信息 |

配置文件与 CLI 可同时使用：**CLI 优先级更高**。

### 配置文件示例

仓库内可参考 [`configs/config.toml`](configs/config.toml)。极简示例：

```toml
[server]
socket_path = "/tmp/feb.sock"
log_level = "info"

[monitor]
path = "/data/sate"
recursive = true
events = ["CLOSE_WRITE"]
exclude_extensions = [".tmp", ".swp"]
exclude_paths = ["/data/sate/cache"]

[processor]
use_io_uring = true
```

使用方式：

```bash
sudo ./FsEventBridge -c /path/to/config.toml
```

### 消费事件（NDJSON）

先启动 FsEventBridge，再在业务侧连接同一 UDS，按行读取 JSON（一行一个对象）。示例客户端：`tests/test_client.py`、`tests/test_client.go`。

每条事件对象的字段：

| 字段 | 含义 |
|------|------|
| `path` | 文件路径 |
| `event` | 可读事件名，如 `CLOSE_WRITE`、`MODIFY` |
| `type` | 与 `event` 对应的整数枚举（`UNKNOWN` = 0，其后按 CLOSE_WRITE、MOVED_TO … 递增） |
| `size` | 当前文件大小（字节） |
| `ts` | **网关观测时间**：处理该 fanotify 事件并完成元数据读取之后的墙钟时刻（Unix **秒**，`CLOCK_REALTIME`） |
| `mtime` | 文件内容最后修改时间（`st_mtim` 秒）；`fstat` 失败时为 `-1` |
| `mask` | fanotify 原始掩码（可与内核 `FAN_*` 对照，例如 `FAN_CLOSE_WRITE` 常为 `8`/`0x8`） |

---

## 🔗 上游集成示例 (Go)

```go
conn, _ := net.Dial("unix", "/tmp/feb.sock")
scanner := bufio.NewScanner(conn)

for scanner.Scan() {
    var event MyFileEvent // 按需映射 path / event / type / ts / mtime / mask 等字段
    json.Unmarshal(scanner.Bytes(), &event)
    processSatelliteData(event.Path)
}
```

---

## 🧪 开发与回归测试

本地构建后：

```bash
bash tests/run.sh --milestone 0 --type unit    # 无需 root
sudo -E bash tests/run.sh --milestone 0 --type e2e
sudo -E bash tests/run.sh --milestone 1 --type e2e   # NDJSON 语义回归
sudo -E bash tests/run.sh --milestone 3 --type e2e     # IPC（需 root）
```

说明见 [`tests/README.md`](tests/README.md)。路线图见 [`DEVELOPMENT_PLAN.md`](DEVELOPMENT_PLAN.md)。

---

## ⚖️ 开源协议

本项目采用 **Apache-2.0** 协议开源。
