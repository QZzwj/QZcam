# 宿主侧渲染探针：用仓库里那份 vendored LVGL + lv_conf.h 编一个小程序，
# 在 PC 上看"LVGL 到底把图片画出来没有"。
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ./build/lv_render_probe /path/to/photo.jpg -2
cmake_minimum_required(VERSION 3.16)
project(qzcam_host_probes C)

set(CMAKE_C_STANDARD 99)

# 与主工程一致：examples/demos 不在 vendored 源码里，得关掉（否则 LVGL 的
# CMake 会因为没有源文件而报错）
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)

set(LVGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/lvgl)
add_subdirectory(${LVGL_DIR} lvgl_build)

# lv_conf.h 就在 third_party/ 下（LVGL 靠 LV_CONF_INCLUDE_SIMPLE 找它）
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

add_executable(lv_render_probe lv_render_probe.c)
# thorvg 是 C++ 的（矢量图那套），链接要带上 stdc++
target_link_libraries(lv_render_probe PRIVATE lvgl lvgl::thorvg m stdc++)
