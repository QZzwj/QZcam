#!/bin/sh
# 截一张板子 LCD 的图：tools/screen.sh [输出.png]
#   原理：adb pull /dev/fb0（RGB565 字节精确）→ 本机转 PNG。
#   对应的连续投屏：tools/remote_desktop.py；远程点按：echo 'tap x y' > /tmp/qz-touch.fifo
set -e
ADB=${QZCAM_ADB:-/mnt/d/adb/adb.exe}
[ -x "$ADB" ] || ADB=$(command -v adb)
OUT=${1:-/tmp/qzcam_screen.png}
TMP=$(mktemp /tmp/qzcam_screen.XXXX.raw)

"$ADB" pull /dev/fb0 "$TMP" >/dev/null 2>&1 || { echo "拉取失败（adb 连上板子了吗）"; exit 1; }
SIZE=$("$ADB" shell cat /sys/class/graphics/fb0/virtual_size 2>/dev/null | tr -d '\r')
W=${SIZE%,*}; H=${SIZE#*,}
[ -n "$W" ] || { W=320; H=240; }

python3 - "$TMP" "$W" "$H" "$OUT" <<'PY'
import sys
from PIL import Image
raw, w, h, out = open(sys.argv[1],'rb').read(), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
assert len(raw) == w*h*2, f"帧大小不符: {len(raw)} != {w*h*2}"
Image.frombytes('RGB', (w, h), raw, 'raw', 'BGR;16').save(out)
print("  已保存", out)
PY
rm -f "$TMP"
