# QZcam

**RV1106**（Luckfox Pico / QZdesk DeskMate）上的独立相机固件：开机直接全屏取景 ——
没有桌面，只有一个程序。基于 [QZdesk SDK](https://github.com/QZzwj/QZSDK) 构建，
交叉工具链、内核、媒体库和它装进去的 `oem` 树都由 SDK 提供。

[English](./README.md)

## 功能

- **取景**：面板上实时预览，拍照和录像共用这一路画面。
- **五种模式**：视频 / 照片 / 慢动作 / 延时 / 循环。
- **拍照**：单张、连拍、定时自拍（延时 + 张数）。
- **相册**：按日期分组；点照片按比例缩放查看，点录像直接回放；可单条删除或全部清空。
- **设置**：分辨率、录像帧率、编码（H.265 / H.264）、画质、码率模式、白平衡（色温）、
  EV 补偿、背光亮度、自动息屏、时间（板上没有 RTC，手动对时）。
- **输入**：只有触摸屏 —— 自动识别带绝对坐标轴的 evdev 设备。

## 链路

| 环节 | 实现 |
|---|---|
| 取景 | ISP：rkaiq 3A + `RK_MPI_VI` → NV12 → LVGL 面板 |
| 录像 | `RK_MPI_VENC` H.265/H.264 → `rkmuxer` 封 MP4；兜底：未压缩 RGB565 AVI |
| 回放 | MP4 走 `rkdemuxer` + `RK_MPI_VDEC`；AVI 直接读块翻行，不解码 |
| 帧来源 | ISP（默认）· 裸 V4L2（`rkisp_mainpath`）· 彩条（`QZCAM_BACKEND=fake`） |

线程：取景工作线程（取帧 + 拍照，三缓冲轮换发布）、VENC 取流线程（码流 → 封装器），
主线程跑 LVGL。

## 硬件

| 项 | 值 |
|---|---|
| 芯片 | RV1106（`arm-rockchip830-linux-uclibcgnueabihf`） |
| 传感器 | SC3336，2304×1296@30（IQ `sc3336_CMK-OT2119-PC1_30IRC-F16`）；640×480@75 需要社区驱动并重启 |
| 屏幕 | 运行时从 `/dev/fb0` 探测，不写死型号；背光走 sysfs，息屏/唤醒走 `FBIOBLANK` |
| 输入 | 触摸屏（evdev） |

## 编译

### 在 SDK 里编（真机常规路径）

SDK 的 `project/app/qzcam` 里有 `src ->` 本仓库的软链，所以 SDK 直接编它：

```bash
cd <SDK>
./build.sh app                 # 交叉编译 → output/out/app_out，安装进 oem 树
tools/build/deploy_qzcam.sh    # adb 推到板子上并重启
```

SDK 侧的 Makefile 会装到 `/oem/usr/bin/qzcam`、字体、`S30qzcam`、相机内核模块和 IQ 文件；
`S30qzcam` 在开机时按序加载驱动栈并拉起本程序。

### 独立 CMake

```bash
# 真机：媒体库取自 SDK 的 media_out
cmake -S . -B build -DQZCAM_MEDIA_DIR=<SDK>/output/out/media_out && cmake --build build -j8

# 开发机 SDL 模拟器（不需要板子）
cmake -S . -B build-sim -DQZCAM_SIMULATOR=ON && cmake --build build-sim -j8
tools/run_sim.sh               # --real 用板载相机，--fake 画彩条
```

不给 `QZCAM_MEDIA_DIR` 时是降级而不是编译失败：录像退回未压缩 AVI，MP4 回放不可用。

## 设备上的文件

| 内容 | 位置 |
|---|---|
| 设置 | `qzcam-settings.txt`（`key=value`），按 `/oem/usr/etc` → `/userdata` → `/tmp` 取第一个可写的 |
| 素材 | `/mnt/sdcard` → `/userdata/qzcam` → `/tmp/qzcam`，支持热插拔（`QZCAM_DIR` 可覆盖） |
| 照片 | `IMG_年月日_时分秒_序号.jpg`，另有 `.jpg.thumb.jpg` / `.jpg.original.jpg` 附件 —— 不是独立照片 |
| 录像 | `VID_年月日_时分秒.avi`（未压缩）或 `.mp4`（硬编码） |

## 目录

| 路径 | 说明 |
|---|---|
| `app/` | 应用本体：界面、ISP/取帧、录像、编码、回放、设置、存储、面板、触摸 |
| `include/` | `qzcam.h`（设置）、`cam_player.h` |
| `third_party/` | LVGL 9.2（与 QZdesk 共用）+ `lv_conf.h`、`conf/dev_conf.h` |
| `tools/` | `run_sim.sh`、`screen.sh`、`remote_desktop.py`、`play.py`、`device/uitap.c`、`host_probes/` |
| `assets/` | 界面字体 |

## 文档

- SDK、板级配置、烧录、部署脚本：[QZdesk SDK](https://github.com/QZzwj/QZSDK)
- 设备侧脚本（在 SDK 树里）：`S30qzcam`、`replace_desktop.sh`、`camera_doctor.sh`、
  `camera_paths_test.sh`
