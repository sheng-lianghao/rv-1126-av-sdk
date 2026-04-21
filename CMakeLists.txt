cmake_minimum_required(VERSION 3.16)
project(av_sdk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------
# 交叉编译：通过 -DCMAKE_TOOLCHAIN_FILE=toolchain-rv1126.cmake 触发
# ------------------------------------------------------------------
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compiling for ${CMAKE_SYSTEM_PROCESSOR} (${CMAKE_SYSTEM_NAME})")
    message(STATUS "  C compiler : ${CMAKE_C_COMPILER}")
    message(STATUS "  Sysroot    : ${CMAKE_SYSROOT}")
endif()

# ------------------------------------------------------------------
# 依赖查找（pkg-config）
# ------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBAVCODEC    REQUIRED libavcodec)
pkg_check_modules(LIBAVFORMAT   REQUIRED libavformat)
pkg_check_modules(LIBAVUTIL     REQUIRED libavutil)
pkg_check_modules(LIBSWRESAMPLE REQUIRED libswresample)
pkg_check_modules(ALSA          REQUIRED alsa)

# V4L2 直接使用内核 uAPI（无需链接额外库）
find_path(V4L2_INCLUDE_DIR linux/videodev2.h
    HINTS ${CMAKE_SYSROOT}/usr/include /usr/include
    REQUIRED
)

# ------------------------------------------------------------------
# SDK 静态库
# ------------------------------------------------------------------
add_library(avsdk STATIC
    src/safe_queue.c
    src/video_capture.c
    src/audio_capture.c
    src/video_encoder.c
    src/audio_encoder.c
    src/muxer.c
    src/pipeline.c
)

target_include_directories(avsdk
    PUBLIC  include
    PRIVATE src
            ${LIBAVCODEC_INCLUDE_DIRS}
            ${LIBAVFORMAT_INCLUDE_DIRS}
            ${LIBAVUTIL_INCLUDE_DIRS}
            ${LIBSWRESAMPLE_INCLUDE_DIRS}
            ${ALSA_INCLUDE_DIRS}
)

target_compile_options(avsdk PRIVATE
    -Wall -Wextra
    -D_GNU_SOURCE
    ${LIBAVCODEC_CFLAGS_OTHER}
    ${ALSA_CFLAGS_OTHER}
)

target_link_libraries(avsdk PUBLIC
    ${LIBAVFORMAT_LIBRARIES}
    ${LIBAVCODEC_LIBRARIES}
    ${LIBAVUTIL_LIBRARIES}
    ${LIBSWRESAMPLE_LIBRARIES}
    ${ALSA_LIBRARIES}
    pthread
    m
)

# ------------------------------------------------------------------
# 示例应用
# ------------------------------------------------------------------
add_executable(av_capture app/main.c)

target_include_directories(av_capture PRIVATE include)
target_link_libraries(av_capture PRIVATE avsdk)

install(TARGETS av_capture DESTINATION bin)
install(FILES include/pipeline.h DESTINATION include/avsdk)
