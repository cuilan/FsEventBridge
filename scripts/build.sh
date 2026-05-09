#!/bin/bash
set -e

# 获取脚本所在目录的绝对路径，确保从项目根目录执行
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$PROJECT_ROOT/build"

# 颜色定义
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}[*] Starting Build Process...${NC}"

# 1. 清理旧构建 (可选，通过参数 -c 控制)
if [[ "$1" == "-c" || "$1" == "--clean" ]]; then
    echo -e "${GREEN}[*] Cleaning build directory...${NC}"
    rm -rf "$BUILD_DIR"
fi

# 2. 创建构建目录
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# 3. 生成 Makefile
echo -e "${GREEN}[*] Configuring CMake...${NC}"
if ! cmake "$PROJECT_ROOT"; then
    echo -e "${RED}[!] CMake configuration failed.${NC}"
    exit 1
fi

# 4. 编译
echo -e "${GREEN}[*] Building project...${NC}"
if ! make -j$(nproc); then
    echo -e "${RED}[!] Build failed.${NC}"
    exit 1
fi

echo -e "${GREEN}[SUCCESS] Build complete. Binaries: $BUILD_DIR/fseventbridge (+ symlink feb)${NC}"
