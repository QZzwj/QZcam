#!/usr/bin/env python3
"""QZcam 在线回放：不用拖文件，直接在电脑上看板子里录的视频/照片。

用法：
  python3 tools/play.py                 # 交互：列出录像/照片，输序号播放
  python3 tools/play.py --list          # 只列清单
  python3 tools/play.py --play 3        # 播第 3 个
  python3 tools/play.py --play VID_0001.avi
  python3 tools/play.py --fresh         # 忽略缓存强制重拉

原理：adb pull 拉到本机缓存（~/.cache/qzcam-play，按 文件名+大小 命中就不再
拉），视频用 ffplay 放，照片用系统看图工具。录像格式是 AVI+RGB565 未压缩，
ffplay/ffmpeg 原生支持。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

DEFAULT_ADB_CANDIDATES = [
    "/mnt/d/adb/adb.exe",          # WSL 里 Linux adb 看不到 USB，优先 Windows 侧
    "/mnt/c/platform-tools/adb.exe",
    shutil.which("adb"),
]
DIRS = ["/mnt/sdcard", "/userdata/qzcam"]   # 与 cam_storage 的候选一致
CACHE = os.path.expanduser("~/.cache/qzcam-play")


def find_adb(user_path):
    if user_path:
        return user_path
    for c in DEFAULT_ADB_CANDIDATES:
        if c and os.path.exists(c):
            return c
    return "adb"


def list_recordings(adb):
    """adb shell ls -l 解析（文本通道，安全）。返回 [(name, path, size)]"""
    items = []
    for d in DIRS:
        out = subprocess.run([adb, "shell", "ls -l %s/" % d],
                             capture_output=True, text=True, timeout=15).stdout
        for line in out.splitlines():
            m = re.match(r"[-rwx]+\s+\S+\s+\S+\s+\S+\s+(\d+)\s+\w+\s+\d+\s+[\d:]+\s+(.*)", line)
            if not m:
                continue
            size, name = int(m.group(1)), m.group(2).strip()
            if name and not name.startswith("."):
                items.append((name, "%s/%s" % (d, name), size))
    return items


def cache_path(name, size):
    os.makedirs(CACHE, exist_ok=True)
    return os.path.join(CACHE, "%s.%d" % (name, size))


def fetch(adb, name, remote, size, fresh=False):
    dst = cache_path(name, size)
    if not fresh and os.path.exists(dst) and os.path.getsize(dst) == size:
        print("  缓存命中：%s" % dst)
        return dst
    tmp = os.path.join(tempfile.gettempdir(), "qzcam_pull.tmp")
    subprocess.run([adb, "pull", remote, tmp], capture_output=True, timeout=300)
    if not os.path.exists(tmp) or os.path.getsize(tmp) != size:
        sys.exit("拉取不完整，重试或检查连接")
    shutil.move(tmp, dst)
    print("  已拉取：%s（%d 字节）" % (dst, size))
    return dst


def play(local):
    if local.lower().endswith((".bmp", ".jpg", ".jpeg", ".png")):
        for opener in ("xdg-open", "wslview"):
            if shutil.which(opener):
                subprocess.Popen([opener, local])
                return
        rundll = "/mnt/c/Windows/System32/rundll32.exe"
        if os.path.exists(rundll):
            subprocess.Popen([rundll, "url.dll,FileProtocolHandler",
                              os.path.abspath(local)])
            return
        subprocess.run(["ffplay", "-loglevel", "quiet", "-loop", "1", local], check=False)
        return
    ffplay = shutil.which("ffplay")
    if not ffplay:
        sys.exit("没找到 ffplay（装 ffmpeg：sudo apt install ffmpeg）")
    subprocess.run([ffplay, "-loglevel", "quiet", "-autoexit", local], check=False)


def pick(items, sel):
    if sel.isdigit():
        idx = int(sel) - 1
        if 0 <= idx < len(items):
            return items[idx]
    for it in items:
        if it[0] == sel or it[0].startswith(sel):
            return it
    return None


def main():
    ap = argparse.ArgumentParser(description="QZcam 在线回放（adb）")
    ap.add_argument("--adb", help="adb 路径（默认自动找）")
    ap.add_argument("--list", action="store_true", help="只列清单")
    ap.add_argument("--play", help="播放：序号或文件名")
    ap.add_argument("--fresh", action="store_true", help="忽略缓存强制重拉")
    args = ap.parse_args()

    adb = find_adb(args.adb)
    items = list_recordings(adb)
    if not items:
        sys.exit("板上没有录像/照片（或 adb 未连接）")

    print("板上的录像/照片：")
    for i, (name, _, size) in enumerate(items, 1):
        print("  %2d. %-28s %8.1f KB" % (i, name, size / 1024))

    if args.list:
        return

    sel = args.play
    if sel is None:
        try:
            sel = input("\n要播哪个（序号/文件名，回车退出）？").strip()
        except (EOFError, KeyboardInterrupt):
            return
    if not sel:
        return
    it = pick(items, sel)
    if not it:
        sys.exit("没找到：%s" % sel)
    name, remote, size = it
    local = fetch(adb, name, remote, size, args.fresh)
    play(local)


if __name__ == "__main__":
    main()
