#!/bin/bash

set -e

# 1. 创建构建目录
mkdir build && cd build

# 2. 生成 Makefile
cmake ..

# 3. 编译
make -j$(nproc)

# 4. 一键打包
cpack