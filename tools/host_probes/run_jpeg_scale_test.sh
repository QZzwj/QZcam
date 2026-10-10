#!/bin/bash
# 跑 jpeg_scale_test：拿真的 cam_image.c（QZCAM_HAVE_JPEG）配假的 libjpeg 实现，
# 在 PC 上验证缩放解码的几何。只需要 SDK 里那份 jpeglib.h，不需要交叉工具链。
#
#   ./run_jpeg_scale_test.sh            # 默认在 $HOME/QZdesk/SDK 找头文件
#   QZCAM_SDK=/path/to/sdk ./run_jpeg_scale_test.sh
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SDK="${QZCAM_SDK:-$HOME/QZdesk/SDK}"

# libjpeg 的头在 SDK 的 third_libs 里（几个候选位置都试一下）
JPEG_INC=""
for d in "$SDK/project/app/component/third_libs/include/libjpeg9" \
         "$SDK/project/app/component/third_libs/out/include/libjpeg9" \
         "$SDK/project/app/out/include/libjpeg9"; do
    if [ -f "$d/jpeglib.h" ]; then JPEG_INC="$d"; break; fi
done
if [ -z "$JPEG_INC" ]; then
    echo "找不到 jpeglib.h；用 QZCAM_SDK=<SDK 根目录> 指一下" >&2
    exit 2
fi
echo "jpeglib.h: $JPEG_INC"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cc -O2 -DQZCAM_HAVE_JPEG=1 -D_GNU_SOURCE \
   -I"$JPEG_INC" -I"$REPO/include" -I"$REPO/app" \
   -c "$REPO/app/cam_image.c" -o "$WORK/cam_image.o"

cc -O2 -I"$JPEG_INC" -I"$HERE" -o "$WORK/jpeg_scale_test" \
   "$HERE/jpeg_scale_test.c" "$HERE/fake_jpeg.c" "$WORK/cam_image.o" -lm

cd "$WORK" && touch fake.jpg && ./jpeg_scale_test
