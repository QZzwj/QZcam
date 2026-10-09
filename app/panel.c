#include "qzcam.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lvgl.h"

int qzcam_panel_w = QZCAM_PANEL_W;
int qzcam_panel_h = QZCAM_PANEL_H;

/* 模拟器：QZCAM_PANEL=480x320；不设就用编译期尺寸。 */
static void from_env(void)
{
    const char *spec = getenv("QZCAM_PANEL");
    int w = 0, h = 0;
    if (spec && sscanf(spec, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        qzcam_panel_w = w;
        qzcam_panel_h = h;
    }
}

/* 真机：问 framebuffer 要实际分辨率 —— LVGL 的 fbdev 驱动读的是同一份 ioctl，
 * 这里先问一次是为了让布局、字体、取景缓冲都按真实尺寸来。 */
static bool from_framebuffer(void)
{
    const char *device = getenv("QZCAM_FB");
    if (!device || !device[0]) device = "/dev/fb0";

    int fd = open(device, O_RDWR);
    if (fd < 0) return false;

    struct fb_var_screeninfo info;
    memset(&info, 0, sizeof(info));
    bool ok = ioctl(fd, FBIOGET_VSCREENINFO, &info) == 0 &&
              info.xres > 0 && info.yres > 0;
    if (ok) {
        qzcam_panel_w = (int)info.xres;
        qzcam_panel_h = (int)info.yres;
    }
    close(fd);
    return ok;
}

void qzcam_panel_init(void)
{
    from_env();
#if LV_USE_SIMULATOR
    /* 模拟器上环境变量是权威来源，不去碰开发机的 /dev/fb0。 */
#else
    if (!from_framebuffer()) {
        fprintf(stderr, "QZcam: 读不到 framebuffer 尺寸，用兜底 %dx%d\n",
                qzcam_panel_w, qzcam_panel_h);
    }
#endif
    fprintf(stderr, "QZcam: 面板 %dx%d\n", qzcam_panel_w, qzcam_panel_h);
}

/* ---- 取景直写 framebuffer ------------------------------------------- *
 * 相机帧绕过 LVGL 直接 memcpy 进 fb（UI 只局部重绘控件），否则每帧全屏
 * 重渲染 + 全屏 SPI 刷写，动效必卡。模拟器没有 fb，返回 NULL 走 lv_image。 */
static uint16_t *fb_map;
static int fb_stride_px;

uint16_t *qzcam_panel_fb_map(int *stride_px)
{
#if LV_USE_SIMULATOR
    (void)stride_px;
    return NULL;
#else
    if (fb_map) {
        if (stride_px) *stride_px = fb_stride_px;
        return fb_map;
    }
    const char *device = getenv("QZCAM_FB");
    if (!device || !device[0]) device = "/dev/fb0";
    int fd = open(device, O_RDWR);
    if (fd < 0) return NULL;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0 ||
        finfo.line_length < (size_t)qzcam_panel_w * 2) {
        close(fd);
        return NULL;
    }
    fb_map = mmap(NULL, (size_t)finfo.smem_len, PROT_READ | PROT_WRITE,
                  MAP_SHARED, fd, 0);
    close(fd);   /* mmap 保留了映射，fd 可以关 */
    if (fb_map == MAP_FAILED) {
        fb_map = NULL;
        return NULL;
    }
    fb_stride_px = (int)(finfo.line_length / 2);
    if (stride_px) *stride_px = fb_stride_px;
    return fb_map;
#endif
}
