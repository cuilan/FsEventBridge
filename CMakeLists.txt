cmake_minimum_required(VERSION 3.12)
project(fseventbridge C)

# --- 自动版本管理 ---
# 读取 VERSION 文件内容
file(READ "VERSION" FEB_VERSION_RAW)
string(STRIP "${FEB_VERSION_RAW}" FEB_VERSION)
message(STATUS "fseventbridge ${FEB_VERSION}")

# 1. 编译器选项：强制 C17 标准并开启严苛模式
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 开启所有警告并视警告为错误，确保代码质量符合工业级标准
add_definitions(-D_GNU_SOURCE -DFEB_VERSION="${FEB_VERSION}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -O2 -g")

# 2. 查找必要的系统库
find_package(PkgConfig REQUIRED)
# 查找 libsystemd (用于 sd_notify)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)
# 查找 liburing (io_uring 核心库)
find_library(LIBURING uring REQUIRED)

# 3. 定义项目源码与包含路径
include_directories(include vendor/tomlc99)
# 包含 tomlc99 源代码 (放在 vendor/tomlc99 下)
add_executable(fseventbridge
    src/main.c
    src/monitor.c
    src/ipc_uds.c
    src/config_loader.c
    src/utils.c
    vendor/tomlc99/toml.c  # tomlc99 解析器
)

# 便捷别名：开发目录内 feb → fseventbridge；安装阶段见下方 install(CODE)
if(UNIX)
    add_custom_command(
        TARGET fseventbridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_BINARY_DIR}/feb"
        COMMAND ${CMAKE_COMMAND} -E create_symlink fseventbridge "${CMAKE_BINARY_DIR}/feb"
        COMMENT "feb → fseventbridge (仅供开发目录内使用)")
endif()

# 链接库：系统库、systemd、uring 和数学库
target_link_libraries(fseventbridge
    ${LIBURING}
    ${SYSTEMD_LIBRARIES}
    m
)

# 4. 安装定义 (决定执行 make install / CPack 预阶段时的去向)
set(FEB_INSTALL_BINDIR "/usr/bin")
install(TARGETS fseventbridge RUNTIME DESTINATION "${FEB_INSTALL_BINDIR}")

# 安装后 usr/bin/feb → fseventbridge（不需要时可删除本段）
install(CODE "
  set(_destdir \"\$ENV{DESTDIR}\")
  set(_feb \"\${_destdir}${FEB_INSTALL_BINDIR}/feb\")
  if(EXISTS \"\${_feb}\")
    file(REMOVE \"\${_feb}\")
  endif()
  execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink fseventbridge \"\${_feb}\" RESULT_VARIABLE _feb_sym)
  if(NOT _feb_sym EQUAL 0)
    message(FATAL_ERROR \"create symlink feb -> fseventbridge failed (\${_feb_sym})\")
  endif()
")

install(FILES configs/config.toml DESTINATION /etc/fseventbridge)
install(FILES package/fseventbridge.service DESTINATION /lib/systemd/system)

# --- 5. 测试集成 (ctest) ---
# 通过 tests/run.sh 串联各里程碑的 unit / e2e 测试
# unit:   不需要 root，直接跑
# e2e:    需要 fanotify 权限，沙箱 / 受限环境会自动 SKIP
enable_testing()

add_test(
    NAME milestone0_unit
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run.sh --milestone 0 --type unit
)
set_tests_properties(milestone0_unit PROPERTIES
    LABELS "milestone0;unit"
    SKIP_RETURN_CODE 77
    ENVIRONMENT "FEB_BIN=$<TARGET_FILE:fseventbridge>;BUILD_DIR=${CMAKE_BINARY_DIR}"
)

add_test(
    NAME milestone0_e2e
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run.sh --milestone 0 --type e2e
)
set_tests_properties(milestone0_e2e PROPERTIES
    LABELS "milestone0;e2e"
    SKIP_RETURN_CODE 77
    ENVIRONMENT "FEB_BIN=$<TARGET_FILE:fseventbridge>;BUILD_DIR=${CMAKE_BINARY_DIR}"
)

add_test(
    NAME milestone1_e2e
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run.sh --milestone 1 --type e2e
)
set_tests_properties(milestone1_e2e PROPERTIES
    LABELS "milestone1;e2e"
    SKIP_RETURN_CODE 77
    ENVIRONMENT "FEB_BIN=$<TARGET_FILE:fseventbridge>;BUILD_DIR=${CMAKE_BINARY_DIR}"
)

add_test(
    NAME milestone3_e2e
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run.sh --milestone 3 --type e2e
)
set_tests_properties(milestone3_e2e PROPERTIES
    LABELS "milestone3;e2e"
    SKIP_RETURN_CODE 77
    ENVIRONMENT "FEB_BIN=$<TARGET_FILE:fseventbridge>;BUILD_DIR=${CMAKE_BINARY_DIR}"
)

add_test(
    NAME milestone4_e2e
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run.sh --milestone 4 --type e2e
)
set_tests_properties(milestone4_e2e PROPERTIES
    LABELS "milestone4;e2e"
    SKIP_RETURN_CODE 77
    ENVIRONMENT "FEB_BIN=$<TARGET_FILE:fseventbridge>;BUILD_DIR=${CMAKE_BINARY_DIR}"
)

# --- 6. CPack 自动打包配置 (生成 .deb 和 .rpm) ---

set(CPACK_PACKAGE_NAME "fseventbridge")
set(CPACK_PACKAGE_VERSION "${FEB_VERSION}")
set(CPACK_PACKAGE_VENDOR "fseventbridge")
set(CPACK_PACKAGE_CONTACT "admin@example.com")
set(CPACK_PACKAGE_DESCRIPTION "High-performance Linux file system event gateway via fanotify.")

# 指定生成器
set(CPACK_GENERATOR "DEB;RPM")

# Debian (.deb) 特有配置
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "YourName")
# 运行时依赖：确保用户安装包时会自动安装 liburing 和 systemd 运行环境
set(CPACK_DEBIAN_PACKAGE_DEPENDS "liburing2, libc6, libsystemd0")
# 核心：安装后自动执行脚本 (见下方)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${CMAKE_CURRENT_SOURCE_DIR}/package/postinst;${CMAKE_CURRENT_SOURCE_DIR}/package/prerm;${CMAKE_CURRENT_SOURCE_DIR}/package/postrm")

# RPM (.rpm) 特有配置
set(CPACK_RPM_PACKAGE_REQUIRES "liburing, glibc, systemd")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/package/postinst")

include(CPack)
