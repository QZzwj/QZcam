#!/usr/bin/env python3
"""QZcam 远程桌面：把板子的 LCD（/dev/fb0，RGB565）通过 adb 投到电脑上，
**鼠标就是触摸屏** —— 等效 scrcpy 的体验（板子不是 Android，scrcpy 用不了，
这里用 帧缓冲直读 + uitap 虚拟触摸屏 等效实现）。

用法：
  tools/remote_desktop.py                  # 一体化窗口：实时画面 + 点击/拖动注入（需 pygame）
  tools/remote_desktop.py --rate 10        # 目标帧率
  tools/remote_desktop.py --adb /mnt/d/adb/adb.exe
  tools/remote_desktop.py --noinput        # 只看不动（免装 pygame，走 ffplay）
  tools/remote_desktop.py --out /tmp/rd --frames 5   # 抓 N 帧存 PNG（CI/验收）

操作（pygame 窗口内）：
  鼠标左键点击   → tap（点击屏幕坐标）
  按住拖动       → swipe（按下→松开整段轨迹）
  Ctrl+C / 关窗  → 退出

前提：adb 已连板；板上 uitap 在跑（/tmp/qz-touch.fifo 存在，固件自带）。
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


class TouchInjector:
    """把窗口里的鼠标事件转成板上的 tap/swipe（写 uitap 的 fifo）。"""

    def __init__(self, adb, panel_w, panel_h):
        self.adb = adb
        self.pw = panel_w
        self.ph = panel_h
        self.down = None   # (x, y) 按下点（面板坐标）

    def _send(self, cmd):
        # 注意尾巴上的 sleep：实测 adb 会话一关，板上 shell 会被立刻拆掉，
        # echo 的数据还没交给 uitap 就丢了（症状：退出码 0 但界面没反应）。
        # 留 0.5s 让写入落地。板上实测：不带 sleep 无效，带 sleep 生效。
        try:
            subprocess.run([self.adb, "shell",
                            "echo '%s' > /tmp/qz-touch.fifo; sleep 0.5" % cmd],
                           capture_output=True, timeout=8)
        except Exception:
            pass

    def _map(self, px, py, win_w, win_h):
        """窗口像素 → 面板坐标（窗口可能被放大了）。"""
        return (int(px * self.pw / max(win_w, 1)),
                int(py * self.ph / max(win_h, 1)))

    def on_down(self, px, py, win_w, win_h):
        self.down = self._map(px, py, win_w, win_h)

    def on_up(self, px, py, win_w, win_h):
        if self.down is None:
            return
        end = self._map(px, py, win_w, win_h)
        dx, dy = end[0] - self.down[0], end[1] - self.down[1]
        if abs(dx) < 12 and abs(dy) < 12:
            cmd = "tap %d %d" % end
        else:
            cmd = "swipe %d %d %d %d 250" % (self.down[0], self.down[1], end[0], end[1])
        self.down = None
        # 后台线程发：注入里有 0.5s 的会话保持，别让画面跟着卡半秒
        import threading
        threading.Thread(target=self._send, args=(cmd,), daemon=True).start()


def grab_frame(adb, tmp):
    subprocess.run([adb, "pull", "/dev/fb0", tmp], capture_output=True, timeout=10)
    try:
        return open(tmp, "rb").read()
    except FileNotFoundError:
        return b""


def run_gui(adb, w, h, rate, frame_bytes):
    """一体化窗口：画面 + 鼠标注入。"""
    import numpy
    import pygame

    pygame.init()
    scale = max(1, min(3, 720 // h))   # 小面板放大到合适大小，别糊成邮票
    win = pygame.display.set_mode((w * scale, h * scale))
    pygame.display.set_caption("QZcam 远程桌面（点击=触摸，拖动=滑动）")
    clock = pygame.time.Clock()
    injector = TouchInjector(adb, w, h)
    tmp = os.path.join(tempfile.gettempdir(), "qzcam_fb_pull.raw")
    print("  窗口 %dx%d（面板 %dx%d ×%d）—— 左键点按，拖动=滑动" %
          (w * scale, h * scale, w, h, scale))

    running = True
    ok = bad = 0
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                injector.on_down(event.pos[0], event.pos[1],
                                 win.get_width(), win.get_height())
            elif event.type == pygame.MOUSEBUTTONUP and event.button == 1:
                injector.on_up(event.pos[0], event.pos[1],
                               win.get_width(), win.get_height())

        data = grab_frame(adb, tmp)
        if len(data) == frame_bytes:
            px = numpy.frombuffer(data, dtype=numpy.uint16).reshape(h, w)
            r = ((px >> 11) & 0x1F) << 3
            g = ((px >> 5) & 0x3F) << 2
            b = (px & 0x1F) << 3
            rgb = numpy.dstack((r, g, b)).transpose(1, 0, 2)   # (w,h,3)
            surf = pygame.surfarray.make_surface(rgb)
            if scale > 1:
                surf = pygame.transform.scale(surf, (w * scale, h * scale))
            win.blit(surf, (0, 0))
            pygame.display.flip()
            ok += 1
        else:
            bad += 1
        clock.tick(rate)
    pygame.quit()
    print(f"  结束：{ok} 帧，跳过 {bad} 帧")


def run_ffplay(adb, w, h, rate, frame_bytes):
    """只看模式：喂 ffplay（没有 pygame 时）。"""
    ffplay = shutil.which("ffplay")
    if not ffplay:
        sys.exit("没找到 ffplay（--noinput 模式需要，或 pip3 install pygame numpy 用一体化窗口）")
    player = subprocess.Popen(
        [ffplay, "-loglevel", "quiet", "-f", "rawvideo", "-pixel_format", "rgb565le",
         "-video_size", f"{w}x{h}", "-framerate", str(rate), "-i", "-",
         "-window_title", "QZcam 远程桌面（只看；点按请用 pygame 模式）"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
    tmp = os.path.join(tempfile.gettempdir(), "qzcam_fb_pull.raw")
    print("  投屏中（只看），Ctrl+C 退出")
    ok = bad = 0
    try:
        while player.poll() is None:
            data = grab_frame(adb, tmp)
            if len(data) != frame_bytes:
                bad += 1
                continue
            try:
                player.stdin.write(data)
                player.stdin.flush()
                ok += 1
            except BrokenPipeError:
                break
            time.sleep(1.0 / max(rate, 0.5))
    except KeyboardInterrupt:
        pass
    finally:
        try:
            player.stdin.close()
        except Exception:
            pass
        player.terminate()
    print(f"  结束：成功 {ok} 帧，跳过 {bad} 帧")


def main():
    ap = argparse.ArgumentParser(description="QZcam 远程桌面（adb + /dev/fb0 + uitap）")
    ap.add_argument("--adb", help="adb 路径（默认自动找）")
    ap.add_argument("--size", help="面板尺寸，如 320x240（默认从板子读）")
    ap.add_argument("--rate", type=float, default=8.0, help="目标帧率，默认 8")
    ap.add_argument("--noinput", action="store_true", help="只看模式（ffplay，不注入触摸）")
    ap.add_argument("--out", help="不开窗口，把 N 帧存成 PNG 到这个目录")
    ap.add_argument("--frames", type=int, default=5, help="--out 模式抓几帧，默认 5")
    args = ap.parse_args()

    adb = find_adb(args.adb)
    w, h = read_panel(adb, args.size)
    frame_bytes = w * h * 2
    print(f"QZcam 远程桌面: {adb}  面板 {w}x{h}  目标 {args.rate:.1f}fps")

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        from PIL import Image
        tmp = os.path.join(tempfile.gettempdir(), "qzcam_fb_pull.raw")
        for saved in range(1, args.frames + 1):
            while True:
                data = grab_frame(adb, tmp)
                if len(data) == frame_bytes:
                    break
            path = os.path.join(args.out, f"frame_{saved:02d}.png")
            Image.frombytes("RGB", (w, h), data, "raw", "BGR;16").save(path)
            print(f"  已存 {path}")
            time.sleep(1.0 / max(args.rate, 0.5))
        return

    if args.noinput:
        run_ffplay(adb, w, h, args.rate, frame_bytes)
        return

    try:
        import numpy   # noqa: F401
        import pygame  # noqa: F401
    except ImportError as e:
        print(f"  （缺 {e.name}：pip3 install pygame numpy；或加 --noinput 用只看模式）")
        run_ffplay(adb, w, h, args.rate, frame_bytes)
        return

    run_gui(adb, w, h, args.rate, frame_bytes)


if __name__ == "__main__":
    main()
