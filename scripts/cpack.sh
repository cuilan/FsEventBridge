#!/bin/bash
set -e

# 获取脚本所在目录的绝对路径
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$SCRIPT_DIR/.."
BUILD_DIR="$PROJECT_ROOT/build"

# 颜色定义
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}[*] Starting Packaging Process...${NC}"

# 1. 确保先执行构建
if [ ! -f "$BUILD_DIR/FsEventBridge" ]; then
    echo -e "${YELLOW}[!] Executable not found. Running build script first...${NC}"
    "$SCRIPT_DIR/build.sh"
    if [ $? -ne 0 ]; then
        echo -e "${RED}[!] Build failed, cannot proceed with packaging.${NC}"
        exit 1
    fi
fi

cd "$BUILD_DIR"

# 2. 执行 CPack 打包
echo -e "${GREEN}[*] Running CPack...${NC}"

# 检测是否安装了 rpmbuild
if ! command -v rpmbuild &> /dev/null; then
    echo -e "${YELLOW}[!] 'rpmbuild' not found. RPM package generation will be skipped/failed.${NC}"
    echo -e "${YELLOW}[!] To install on Debian/Ubuntu: sudo apt-get install rpm${NC}"
    # 我们可以选择只生成 DEB 以避免报错
    # cpack -G DEB
else
    # 默认生成所有配置的格式
    :
fi

if cpack; then
    echo -e "${GREEN}[SUCCESS] Packaging complete.${NC}"
    echo -e "${GREEN}[*] Packages found in $BUILD_DIR:${NC}"
    ls -lh *.deb *.rpm 2>/dev/null || true
else
    echo -e "${RED}[!] Packaging failed.${NC}"
    # 如果是因为 RPM 失败但 DEB 成功了，仍然算部分成功
    if ls *.deb 1> /dev/null 2>&1; then
        echo -e "${YELLOW}[*] However, .deb package seems to be generated:${NC}"
        ls -lh *.deb
    fi
    exit 1
fi
