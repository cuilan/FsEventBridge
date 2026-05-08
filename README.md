# FsEventBridge (FEB)

FsEventBridge 是一款基于 Linux 内核 `fanotify` `io_uring` 等技术开发的高性能文件系统事件网关。它旨在解决大规模文件监控场景下的性能瓶颈，并将底层内核事件转换为易于消费的 JSON 流，通过 `Unix Domain Socket (UDS)` 跨语言分发给 Go、Java、Python 等上游业务逻辑。

---

## 🚀 核心特性

* **内核级递归监控**：利用 `fanotify` 机制，支持对整个挂载点或大型目录树进行实时监控，无需像 `inotify` 那样手动递归添加监听。
* **NFS 文件监控**：无需 NFS 服务端支持，实现对本机 NFS 客户端的文件监控。
* **极致性能**：采用 **C17 标准** 编写，集成 `io_uring` 实现异步 I/O，确保在每秒产生数千个文件的卫星接收等工业场景下依然保持极低的 CPU 和内存占用。
* **跨语言集成**：通过 Unix Domain Socket 发送 **NDJSON（每行一条 JSON）**，便于 Go、Python、Java 等语言消费。
* **工业级部署**：原生支持 **Systemd**，提供 `.deb` / `.rpm` 打包路径，贴合常见 Linux 运维方式。
* **灵活配置**：支持命令行参数与 **TOML** 配置文件，适合脚本与常驻服务两种用法。

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

先启动 FsEventBridge，再在业务侧连接同一 UDS，按行读取 JSON。仓库提供示例客户端：`tests/test_client.py`、`tests/test_client.go`。

---

## 🔗 上游集成示例 (Go)

```go
conn, _ := net.Dial("unix", "/tmp/feb.sock")
scanner := bufio.NewScanner(conn)

for scanner.Scan() {
    var event MyFileEvent
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
```

说明见 [`tests/README.md`](tests/README.md)。路线图见 [`DEVELOPMENT_PLAN.md`](DEVELOPMENT_PLAN.md)。

---

## ⚖️ 开源协议

本项目采用 **Apache-2.0** 协议开源。
