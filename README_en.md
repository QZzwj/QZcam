# QZcam

Standalone camera firmware for the **RV1106** (Luckfox Pico / QZdesk DeskMate): the board
boots straight into a full-screen viewfinder — no desktop, a single app. Built on the
[QZdesk SDK](https://github.com/QZzwj/QZSDK), which supplies the toolchain, kernel, media
libraries and the `oem` tree it installs into.

[简体中文](./README.md)

## What it does

- **Live viewfinder** on the panel, snapshot and record from the same pipeline.
- **Five modes**: Video / Photo / Slow-motion / Time-lapse / Loop.
- **Photo**: single, burst, self-timer (delay + count).
- **Gallery**: entries grouped by date, tap a photo to view it scaled to the screen, tap a
  video to play it back, delete one or wipe all.
- **Settings**: resolution, record fps, codec (H.265 / H.264), quality, rate-control mode,
  white balance (Kelvin), EV compensation, panel brightness, auto-sleep, clock (no RTC on
  board, so time is set by hand).
- **Input**: touchscreen only — the app auto-detects an absolute-axis evdev device.

## Pipeline

| Stage | How |
|---|---|
| Preview | ISP: rkaiq 3A + `RK_MPI_VI` → NV12 → LVGL panel |
| Record | `RK_MPI_VENC` H.265/H.264 → `rkmuxer` MP4. Fallback: uncompressed RGB565 AVI |
| Playback | MP4: `rkdemuxer` + `RK_MPI_VDEC`. AVI: read blocks + row flip, no decoder |
| Frame source | ISP (default) · raw V4L2 (`rkisp_mainpath`) · fake colour bars (`QZCAM_BACKEND=fake`) |

Threads: a preview worker (capture + snapshot, publishes a 3-buffer rotation), a VENC reader
(VENC stream → muxer), and the main thread running LVGL.

## Hardware

| | |
|---|---|
| SoC | RV1106 (`arm-rockchip830-linux-uclibcgnueabihf`) |
| Sensor | SC3336, 2304×1296@30 (IQ `sc3336_CMK-OT2119-PC1_30IRC-F16`); 640×480@75 needs the community driver and a reboot |
| Panel | probed at runtime from `/dev/fb0` — no model hardcoded; backlight via sysfs, sleep/wake via `FBIOBLANK` |
| Input | touchscreen (evdev) |

## Build

### Inside the SDK (the normal device build)

`project/app/qzcam` in the SDK has `src ->` this repo, so the SDK compiles it directly:

```bash
cd <SDK>
./build.sh app                 # cross-compile → output/out/app_out, staged into the oem tree
tools/build/deploy_qzcam.sh    # adb-push to a live board and restart
```

The SDK-side Makefile installs to `/oem/usr/bin/qzcam`, fonts, `S30qzcam`, the camera
kernel modules and the IQ files; `S30qzcam` loads the driver stack at boot and starts the app.

### Standalone CMake

```bash
# real hardware: media libraries come from the SDK's media_out
cmake -S . -B build -DQZCAM_MEDIA_DIR=<SDK>/output/out/media_out && cmake --build build -j8

# desktop SDL simulator (no board needed)
cmake -S . -B build-sim -DQZCAM_SIMULATOR=ON && cmake --build build-sim -j8
tools/run_sim.sh               # --real uses the board camera, --fake draws colour bars
```

Without `QZCAM_MEDIA_DIR` the build degrades instead of failing: recording falls back to
uncompressed AVI and MP4 playback is unavailable.

## Files on the device

| What | Where |
|---|---|
| Settings | `qzcam-settings.txt` (`key=value`), first writable of `/oem/usr/etc` → `/userdata` → `/tmp` |
| Footage | `/mnt/sdcard` → `/userdata/qzcam` → `/tmp/qzcam`, hot-plug aware (`QZCAM_DIR` overrides) |
| Photos | `IMG_YYYYMMDD_hhmmss_NNNNNN.jpg`, plus `.jpg.thumb.jpg` / `.jpg.original.jpg` sidecars — not separate photos |
| Videos | `VID_YYYYMMDD_hhmmss.avi` (uncompressed) or `.mp4` (hardware-encoded) |

## Layout

| Path | What |
|---|---|
| `app/` | the app: UI, ISP/capture, record, encoder, player, settings, storage, panel, touch |
| `include/` | `qzcam.h` (settings), `cam_player.h` |
| `third_party/` | LVGL 9.2 (shared with QZdesk) + `lv_conf.h`, `conf/dev_conf.h` |
| `tools/` | `run_sim.sh`, `screen.sh`, `remote_desktop.py`, `play.py`, `device/uitap.c`, `host_probes/` |
| `assets/` | UI font |

## Docs

- SDK, board config, flashing, deploy scripts: [QZdesk SDK](https://github.com/QZzwj/QZSDK)
- Device-side helpers (in the SDK tree): `S30qzcam`, `replace_desktop.sh`,
  `camera_doctor.sh`, `camera_paths_test.sh`
