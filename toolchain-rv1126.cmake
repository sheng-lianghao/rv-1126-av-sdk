# Cross-compilation toolchain for RV1126 (ARM Cortex-A7, Linux)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_DIR    /opt/atk-dlrv1126-toolchain/bin)
set(TOOLCHAIN_PREFIX arm-linux-gnueabihf)

# 正点原子 buildroot sysroot（含 FFmpeg、ALSA 等预编译库）
set(CMAKE_SYSROOT /opt/atk-dlrv1126-toolchain/arm-buildroot-linux-gnueabihf/sysroot)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_AR           ${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}-ar)
set(CMAKE_STRIP        ${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}-strip)

# 只在 sysroot 中查找库和头文件，不混入宿主机路径
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config 指向 sysroot 内的 .pc 文件，不使用宿主机的
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}      "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH}        "")
