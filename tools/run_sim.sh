#!/usr/bin/env bash
#
# 在开发机上跑 QZcam（LVGL 的 SDL 模拟器后端）。
#
# 用法：
#   tools/run_sim.sh                     彩条测试画面（默认，不需要摄像头）
#   tools/run_sim.sh --real              试真机取景链路（会去开 /dev/videoN，开发机上一般没有）
#   QZCAM_PANEL=480x320 tools/run_sim.sh 换面板尺寸
#   QZCAM_FPS=30 ...                     改默认帧率（界面里也能拖）
#
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=${QZCAM_BUILD_DIR:-"$repo_dir/build-sim"}

backend=fake
for arg in "$@"; do
    case "$arg" in
        --real) backend=v4l2 ;;
        --fake) backend=fake ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    esac
done

cmake -S "$repo_dir" -B "$build_dir" -DQZCAM_SIMULATOR=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build_dir" -j"$(nproc 2>/dev/null || echo 2)"

export QZCAM_BACKEND=${QZCAM_BACKEND:-$backend}
export QZCAM_PANEL=${QZCAM_PANEL:-320x240}
export QZCAM_DIR=${QZCAM_DIR:-/tmp/qzcam-photos}

exec "$build_dir/qzcam" "$@"
