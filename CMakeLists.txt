cmake_minimum_required(VERSION 3.12.4)
# CXX 是 LVGL 的 thorvg（TinyVG/SVG/Lottie 渲染后端）带的 —— 它内部是 C++，
# 链接时要 stdc++，所以项目里必须启用 CXX。
project(qzcam C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON)
set(LV_CONF_BUILD_DISABLE_DEMOS ON)
option(QZCAM_SIMULATOR "Build the SDL simulator instead of the device target" OFF)

# LVGL 的软件渲染是逐像素循环，-O0 会明显卡（滚动时掉帧），默认 Release。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 面板尺寸的编译期兜底：真机是运行时读 /dev/fb0，模拟器读 QZCAM_PANEL，
# 这两个值只在两者都取不到时生效。
set(QZCAM_PANEL_W 320 CACHE STRING "Panel width fallback")
set(QZCAM_PANEL_H 240 CACHE STRING "Panel height fallback")

# LVGL 与 QZdesk 共用同一份 vendored 源码：third_party 是指向 QZdesk-Demo 的软链。
# 这样两边的 LVGL 版本与 lv_conf.h 只有一份，不会各自漂移。
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl")
set(LVGL_CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
add_subdirectory("${LVGL_DIR}" lvgl)
target_include_directories(lvgl PUBLIC "${LVGL_CONF_DIR}")

if(QZCAM_SIMULATOR)
    find_package(SDL2 REQUIRED)
    target_compile_definitions(lvgl PUBLIC QZDESK_SIMULATOR=1)
endif()

target_compile_definitions(lvgl PUBLIC
    QZCAM_PANEL_W=${QZCAM_PANEL_W}
    QZCAM_PANEL_H=${QZCAM_PANEL_H})

add_executable(qzcam
    app/main.c
    app/panel.c
    app/font.c
    app/cam_core.c
    app/cam_isp.c
    app/touch.c
    app/cam_storage.c
    app/cam_record.c
    app/cam_settings.c
    app/cam_ui.c
)
target_include_directories(qzcam PRIVATE include app)
target_link_libraries(qzcam PRIVATE lvgl lvgl::thorvg m pthread stdc++)

# 真机的 ISP 取景（cam_isp.c）要链 Rockchip 的 rkaiq / rockit / mpp。
# SDK 的工程 Makefile 会把 -DQZCAM_MEDIA_DIR=<SDK>/output/out/media_out 传进来；
# 没有这个变量（模拟器/裸 cmake 构建）时 cam_isp.c 走桩实现，不引任何 Rockchip 头。
if(QZCAM_MEDIA_DIR)
    target_compile_definitions(qzcam PRIVATE QZCAM_HAVE_MEDIA=1)
    target_include_directories(qzcam PRIVATE "${QZCAM_MEDIA_DIR}/include")

    # rkaiq 的头互相引用得毫无规律（"common/xxx.h"、"algos/amerge/yyy.h"、
    # 直接 "RkAiqCalibDbTypes.h"、甚至 "base/xcam_common.h" 这种相对 xcore 的
    # 写法）。手工列目录会一直漏，这里把它底下**所有目录**都加进搜索路径。
    file(GLOB_RECURSE QZCAM_RKAIQ_TREE LIST_DIRECTORIES true
         "${QZCAM_MEDIA_DIR}/include/rkaiq/*")
    # rkaiq 根本身也要在（形如 "common/rk-camera-module.h" 的引用是相对它的）
    set(QZCAM_RKAIQ_INCLUDE_DIRS "${QZCAM_MEDIA_DIR}/include/rkaiq")
    foreach(item ${QZCAM_RKAIQ_TREE})
        if(IS_DIRECTORY "${item}")
            list(APPEND QZCAM_RKAIQ_INCLUDE_DIRS "${item}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES QZCAM_RKAIQ_INCLUDE_DIRS)
    target_include_directories(qzcam PRIVATE ${QZCAM_RKAIQ_INCLUDE_DIRS})
    target_link_directories(qzcam PRIVATE "${QZCAM_MEDIA_DIR}/lib")
    # 顺序有讲究：rockit/mpp 在前（rkaiq 的 3A 回调会用到它们），rkaiq 在后
    target_link_libraries(qzcam PRIVATE rockit rockchip_mpp rkaiq rga)
    message(STATUS "QZcam: ISP 取景已启用，媒体库目录 ${QZCAM_MEDIA_DIR}")
else()
    message(STATUS "QZcam: 未提供 QZCAM_MEDIA_DIR —— cam_isp 走桩（模拟器/无 Rockchip 库构建）")
endif()

if(QZCAM_SIMULATOR)
    target_link_libraries(qzcam PRIVATE SDL2::SDL2)
endif()

message(STATUS "QZcam: 面板兜底 ${QZCAM_PANEL_W}x${QZCAM_PANEL_H}，模拟器=${QZCAM_SIMULATOR}")
