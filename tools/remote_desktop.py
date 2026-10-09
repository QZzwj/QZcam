#!/usr/bin/env python3
"""QZcam 远程桌面：把板子的 LCD（/dev/fb0，RGB565）通过 adb 投到电脑上。

为什么不是 scrcpy：scrcpy 只能投 Android 设备（要 SurfaceFlinger 截屏 +
MediaCodec 编码 + Android 输入注入），板子是 Buildroot Linux，一条都不满足。
这里是等效做法：**帧缓冲直读 + LVGL 的虚拟触摸屏**，看和点都不缺。

用法：
  tools/remote_desktop.py                      # 打开实时窗口（ffplay，约 3~6fps）
  tools/remote_desktop.py --rate 8             # 目标帧率（每帧一次 adb pull）
  tools/remote_desktop.py --adb /mnt/d/adb/adb.exe
  tools/remote_desktop.py --out /tmp/rd --frames 5   # 不开窗口，抓 N 帧存 PNG（CI/验收）

远程点按（配合板上的 uitap 虚拟触摸屏，面板 320x240）：
  adb shell "echo 'tap 160 217' > /tmp/qz-touch.fifo"    # 快门
  adb shell "echo 'tap 288 225' > /tmp/qz-touch.fifo"    # 参数
  adb shell "echo 'swipe 160 200 160 100 400' > /tmp/qz-touch.fifo"
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

DEFAULT_ADB_CANDIDATES = [
    # 顺序有讲究：WSL 里的 Linux adb 看不到 USB 设备（没有 usbipd），
    # 必须优先用 Windows 侧的 adb.exe（WSL 可以直接调它）。
    "/mnt/d/adb/adb.exe",
    "/mnt/c/platform-tools/adb.exe",
    shutil.which("adb"),
]


def find_adb(user_path):
    if user_path:
        return user_path
    for candidate in DEFAULT_ADB_CANDIDATES:
        if candidate and os.path.exists(candidate):
            return candidate
    return "adb"


def read_panel(adb, size_arg):
    """面板尺寸：命令行 > 板子 fb0 的 virtual_size > 默认 320x240。"""
    if size_arg:
        w, h = size_arg.split("x")
        return int(w), int(h)
    try:
        out = subprocess.run([adb, "shell", "cat",
                              "/sys/class/graphics/fb0/virtual_size"],
                             capture_output=True, text=True, timeout=8).stdout.strip()
        w, h = out.replace("\r", "").split(",")
        return int(w), int(h)
    except Exception:
        return 320, 240


def main():
    ap = argparse.ArgumentParser(description="QZcam 远程桌面（adb + /dev/fb0）")
    ap.add_argument("--adb", help="adb 路径（默认自动找）")
    ap.add_argument("--size", help="面板尺寸，如 320x240（默认从板子读）")
    ap.add_argument("--rate", type=float, default=5.0, help="目标帧率，默认 5")
    ap.add_argument("--out", help="不开窗口，把 N 帧存成 PNG 到这个目录")
    ap.add_argument("--frames", type=int, default=5, help="--out 模式抓几帧，默认 5")
    args = ap.parse_args()

    adb = find_adb(args.adb)
    w, h = read_panel(adb, args.size)
    frame_bytes = w * h * 2
    delay = 1.0 / max(args.rate, 0.5)
    print(f"QZcam 远程桌面: {adb}  面板 {w}x{h}  目标 {args.rate:.1f}fps")

    tmp = os.path.join(tempfile.gettempdir(), "qzcam_fb_pull.raw")

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        from PIL import Image
        saved = 0
        while saved < args.frames:
            subprocess.run([adb, "pull", "/dev/fb0", tmp],
                           capture_output=True, timeout=10)
            data = open(tmp, "rb").read()
            if len(data) != frame_bytes:
                continue
            saved += 1
            path = os.path.join(args.out, f"frame_{saved:02d}.png")
            Image.frombytes("RGB", (w, h), data, "raw", "BGR;16").save(path)
            print(f"  已存 {path}")
            time.sleep(delay)
        return

    ffplay = shutil.which("ffplay") or shutil.which("ffmpeg")
    if not ffplay:
        sys.exit("没找到 ffplay/ffmpeg（--out 模式只需要 Pillow）")

    if os.path.basename(ffplay) == "ffplay":
        player = [ffplay, "-loglevel", "quiet", "-f", "rawvideo",
                  "-pixel_format", "rgb565le", "-video_size", f"{w}x{h}",
                  "-framerate", str(args.rate), "-i", "-",
                  "-window_title", "QZcam 远程桌面"]
    else:  # 只有 ffmpeg 时落盘成 mp4（Ctrl+C 停止）
        player = [ffplay, "-y", "-loglevel", "error", "-f", "rawvideo",
                  "-pixel_format", "rgb565le", "-video_size", f"{w}x{h}",
                  "-framerate", str(args.rate), "-i", "-",
                  "-c:v", "libx264", "-preset", "ultrafast", "/tmp/qzcam_rd.mp4"]

    player_proc = subprocess.Popen(player, stdin=subprocess.PIPE,
                                   stdout=subprocess.DEVNULL)
    print("  投屏中，Ctrl+C 退出。远程点按见文件头注释。")
    ok = bad = 0
    try:
        while player_proc.poll() is None:
            subprocess.run([adb, "pull", "/dev/fb0", tmp],
                           capture_output=True, timeout=10)
            data = open(tmp, "rb").read()
            if len(data) != frame_bytes:
                bad += 1
                continue
            try:
                player_proc.stdin.write(data)
                player_proc.stdin.flush()
                ok += 1
            except BrokenPipeError:
                break
            time.sleep(delay)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            player_proc.stdin.close()
        except Exception:
            pass
        player_proc.terminate()
    print(f"  结束：成功 {ok} 帧，跳过 {bad} 帧")


if __name__ == "__main__":
    main()
