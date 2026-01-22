cmake_minimum_required(VERSION 3.12)
project(FsEventBridge C)

# 1. 编译器选项：强制 C17 标准并开启严苛模式
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 开启所有警告并视警告为错误，确保代码质量符合工业级标准
add_definitions(-D_GNU_SOURCE)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -O2 -g")

# 2. 查找必要的系统库
find_package(PkgConfig REQUIRED)
# 查找 libsystemd (用于 sd_notify)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)
# 查找 liburing (io_uring 核心库)
find_library(LIBURING uring REQUIRED)

# 3. 定义项目源码与包含路径
include_directories(include)
# 包含 tomlc99 源代码 (假设放在 src/vendor 下或直接在 src 下)
add_executable(FsEventBridge 
    src/main.c 
    src/monitor.c 
    src/ipc_uds.c 
    src/config_loader.c
    src/toml.c  # tomlc99 解析器
)

# 链接库：系统库、systemd、uring 和数学库
target_link_libraries(FsEventBridge 
    ${LIBURING} 
    ${SYSTEMD_LIBRARIES} 
    m
)

# 4. 安装定义 (决定执行 make install 时的去向)
install(TARGETS FsEventBridge DESTINATION /usr/bin)
install(FILES configs/config.toml DESTINATION /etc/FsEventBridge)
install(FILES package/FsEventBridge.service DESTINATION /lib/systemd/system)

# --- 5. CPack 自动打包配置 (生成 .deb 和 .rpm) ---

set(CPACK_PACKAGE_NAME "fseventbridge")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_VENDOR "FsEventBridge Community")
set(CPACK_PACKAGE_CONTACT "admin@example.com")
set(CPACK_PACKAGE_DESCRIPTION "High-performance Linux file system event gateway via fanotify.")

# 指定生成器
set(CPACK_GENERATOR "DEB;RPM")

# Debian (.deb) 特有配置
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "YourName")
# 运行时依赖：确保用户安装包时会自动安装 liburing 和 systemd 运行环境
set(CPACK_DEBIAN_PACKAGE_DEPENDS "liburing2, libc6, libsystemd0")
# 核心：安装后自动执行脚本 (见下方)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/package/postinst")

# RPM (.rpm) 特有配置
set(CPACK_RPM_PACKAGE_REQUIRES "liburing, glibc, systemd")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/package/postinst")

include(CPack)