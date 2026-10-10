#include "qzcam.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

/* ---- 背光 / 息屏 ----------------------------------------------------- */

/* pct: 30..100 → 写 /sys/class/backlight/*/brightness（按 max 折算）。
 * 没有背光节点（模拟器/驱动不支持）时静默忽略。 */
void qzcam_panel_set_backlight(int pct)
{
    if (pct < 30) pct = 30;
    if (pct > 100) pct = 100;
    const char *dir = "/sys/class/backlight";
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *item;
    while ((item = readdir(d)) != NULL) {
        if (item->d_name[0] == '.') continue;
        char max_path[160], br_path[160], val[16];
        snprintf(max_path, sizeof(max_path), "%s/%s/max_brightness", dir, item->d_name);
        snprintf(br_path, sizeof(br_path), "%s/%s/brightness", dir, item->d_name);
        FILE *mf = fopen(max_path, "r");
        if (!mf) continue;
        int max = 0;
        if (fscanf(mf, "%d", &max) != 1 || max <= 0) { fclose(mf); continue; }
        fclose(mf);
        FILE *bf = fopen(br_path, "w");
        if (!bf) continue;
        snprintf(val, sizeof(val), "%d", max * pct / 100);
        fputs(val, bf);
        fclose(bf);
    }
    closedir(d);
}

/* 息屏/唤醒：FBIOBLANK。返回 0 = 驱动支持。 */
int qzcam_panel_blank(int on)
{
    const char *device = getenv("QZCAM_FB");
    if (!device || !device[0]) device = "/dev/fb0";
    int fd = open(device, O_RDWR);
    if (fd < 0) return -1;
    int ok = ioctl(fd, FBIOBLANK, on ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
    close(fd);
    return ok;
}
