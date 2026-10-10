# 宿主侧探针（host_probes）

在 PC 上回答两个"光读代码猜不出来"的问题：**LVGL 到底把图片画出来没有**、**缩放解码的几何对不对**。
两个探针都用仓库里的真源码（同一份 vendored LVGL / 同一个 `lv_conf.h` / 真的 `cam_image.c`），
所以 PC 上的结论可以直接搬到板子上。

起因：照片查看器一轮黑屏、一轮修完还是黑，都是在上板之后才发现的。这两件事本可以在 PC 上先证死。

---

## 1. `lv_render_probe` —— LVGL 渲染探针

把一张图片喂给 `lv_image`，抓 framebuffer 数像素。

```bash
cd tools/host_probes
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# 造张测试图（16:9，跟相机拍的 1296p 同比例）
ffmpeg -y -f lavfi -i "testsrc2=size=2304x1296" -frames:v 1 -q:v 2 big.jpg

./build/lv_render_probe big.jpg -2     # 现在查看器的做法
./build/lv_render_probe big.jpg 35     # 用 lv_image_set_scale(35) 缩小 —— 会全黑
```

模式：`0`=1:1，`-1`=STRETCH 铺满，`-2`=按源比例给画面框+STRETCH+居中，`>0`=`lv_image_set_scale(值)`。
输出会打印非背景像素数、纵向画面范围，并写出 `out.ppm` 方便肉眼看。

**它证到的结论（LVGL 9.2，软件渲染）：**

| 用例 | 非背景像素 |
|---|---|
| 2304×1296 JPEG，1:1 | 76800/76800（只看到中心一块，正常） |
| 2304×1296 JPEG，`set_scale(35)` | **0/76800 → 全黑** |
| 2304×1296 JPEG，`set_scale(128)` | **0/76800 → 全黑** |
| 2304×1296 JPEG，`set_scale(512)` | 76800/76800（放大正常） |
| `ALIGN_STRETCH` + 对象 320×180 | 正常，rows 30..209（上下留黑 30 行） |

即：**软件缩放只有"放大"管用，"缩小"（scale < 256）对从文件流式解码的源
（JPEG/BMP）一个像素都画不出来**。这就是当初照片预览全黑的原因 —— 所以
`cam_ui.c` 的 `viewer_show_photo()` 现在不走 `lv_image_set_scale`，用
`LV_IMAGE_ALIGN_STRETCH` + 按源宽高比算好的对象尺寸。

---

## 2. `jpeg_scale_test` —— 缩放解码几何测试

验证 `cam_image.c` 的 `qzcam_image_decode_jpeg_rgb565()`：取哪个 1/N、留黑多少、
整幅源是否都映射进来。用**真的 cam_image.c** + **假的 libjpeg**（假解码器按
`scale_num/scale_denom` 吐出一张横向渐变+纵向条带的图，能反推几何）。

```bash
./run_jpeg_scale_test.sh
# 放在别处的 SDK：QZCAM_SDK=/path/to/sdk ./run_jpeg_scale_test.sh
```

只需要 SDK 里那份 `jpeglib.h`，不需要交叉工具链。当前三个用例全过：

| 源 | 画面框 | 留黑 |
|---|---|---|
| 2304×1296（16:9） | 320×180 | 上下各 30 行 |
| 640×480（4:3） | 320×240 | 无 |
| 480×960（竖构图） | 120×240 | 左右各 100 列 |

> 注：`run_jpeg_scale_test.sh` 自己会 build；`build/` 目录与 `out.ppm`、测试图
> 都是产物，不必入库。
