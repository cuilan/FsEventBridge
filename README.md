# FsEventBridge (FEB)

FsEventBridge 是一款基于 Linux 内核 `fanotify` `io_uring` 等技术开发的高性能文件系统事件网关。它旨在解决大规模文件监控场景下的性能瓶颈，并将底层内核事件转换为易于消费的 JSON 流，通过 `Unix Domain Socket (UDS)` 跨语言分发给 Go、Java、Python 等上游业务逻辑。

---

## 🚀 核心特性

* **内核级递归监控**：利用 `fanotify` 机制，支持对整个挂载点或大型目录树进行实时监控，无需像 `inotify` 那样手动递归添加监听。
* **NFS文件监控**：无需 NFS 服务端支持，实现对本机 NFS 客户端的文件监控。
* **极致性能**：采用 **C17 标准** 编写，集成 `io_uring` 实现异步 I/O，确保在每秒产生数千个文件的卫星接收等工业场景下依然保持极低的 CPU 和内存占用。
* **跨语言集成**：通过 `Unix Domain Socket` 发送 **NDJSON (Newline-Delimited JSON)**，完美对接 Go、Python、Java 等高性能微服务架构。
* **工业级部署**：原生支持 **Systemd** 集成，提供标准的 `.deb` 和 `.rpm` 软件包封装，符合企业级运维规范。
* **灵活配置**：支持命令行参数与 **TOML** 配置文件双重驱动，满足自动化脚本与守护进程化运行的不同需求。

---

## 🏗 架构设计

FsEventBridge 作为“桥梁”，将底层复杂的内核调用与上游灵活的业务逻辑解耦：

1. **监听层 (C17/fanotify)**：在内核 VFS 层捕获 FAN_CLOSE_WRITE 事件，确保只有完整落盘的文件才会被触发。
2. **处理层 (io_uring)**：异步读取文件元数据（如大小、解析文件名标识），避免阻塞监控主循环。
3. **分发层 (IPC/UDS)**：将事件封装为 JSON 并推送到 Unix Domain Socket。

---

## 🛠 安装与构建

### 依赖项
* Linux Kernel >= 5.1 (推荐 6.x)
* GCC >= 12 (支持 C17)
* liburing
* libsystemd

### 编译与打包

```Bash
mkdir build && cd build
cmake ..
make

# 生成 .deb 或 .rpm 包
cpack
```

---

## 📖 使用指南

### 命令行模式
快速监控指定目录：

```Bash
./FsEventBridge -d /data/sate -s /tmp/feb.sock
```

### 配置文件模式
使用 TOML 配置文件运行（支持 Systemd 管理）：

```Bash
./FsEventBridge -c /etc/FsEventBridge/config.toml
```

---

## 🔗 上游集成示例 (Go)
由于 FEB 输出标准化的 JSON 流，上游程序可以极其简单地接入：

```Go
// 消费来自 FsEventBridge 的事件
conn, _ := net.Dial("unix", "/tmp/feb.sock")
scanner := bufio.NewScanner(conn)

for scanner.Scan() {
    var event MyFileEvent
    json.Unmarshal(scanner.Bytes(), &event)
    
    // 执行数据解析、业务逻辑
    processSatelliteData(event.Path)
}
```

---

## 📝 配置文件示例 (TOML)

```Ini, TOML
[server]
socket_path = "/tmp/feb.sock"
log_level = "info"

[monitor]
path = "/data/sate"
recursive = true
events = ["CLOSE_WRITE", "MOVED_TO"]
exclude_extensions = [".tmp", ".swp"]

[processor]
# 启用 io_uring 加速
use_io_uring = true
worker_threads = 4
```

## ⚖️ 开源协议
本项目采用 Apache-2.0 协议开源。
