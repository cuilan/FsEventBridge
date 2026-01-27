import socket
import os

# Unix Domain Socket 路径
SOCKET_PATH = "/tmp/feb.sock"

def main():
    if not os.path.exists(SOCKET_PATH):
        print(f"等待 Socket 文件出现: {SOCKET_PATH}...")
    
    # 创建 Unix Socket (SOCK_STREAM)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    
    try:
        client.connect(SOCKET_PATH)
        print(f"成功连接到 {SOCKET_PATH}，正在等待事件...")
        
        while True:
            # 读取数据
            data = client.recv(1024)
            if not data:
                break
            # 打印接收到的 NDJSON 数据
            print(f"[CLIENT] 收到事件: {data.decode('utf-8').strip()}")
    except Exception as e:
        print(f"错误: {e}")
    finally:
        client.close()

if __name__ == "__main__":
    main()
