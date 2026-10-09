/* 触摸屏的自动识别与建立（LVGL evdev 输入）。
 *
 * 板子：FocalTech FTS 电容屏（DTB focaltech@38，i2c + irq/reset gpio），内核里
 * CONFIG_TOUCHSCREEN_FTS=y 内建 → 开机会自己注册 /dev/input/eventN，不用 insmod。
 *
 * 这里的价值是"不猜编号"：扫 /dev/input/event*，谁能报绝对坐标轴（ABS_X/ABS_Y 或
 * ABS_MT_POSITION_X/Y）谁就是触摸屏。 */

#include "touch.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#if LV_USE_SIMULATOR

/* 开发机是 SDL 鼠标（main.c 里建），这里全空实现。 */
void qzcam_touch_attach(lv_display_t *display) { (void)display; }
void qzcam_touch_poll(void) {}
const char *qzcam_touch_device(void) { return "模拟器（SDL 鼠标）"; }
bool qzcam_touch_ready(void) { return true; }
void qzcam_touch_scan(char *out, unsigned size)
{
    snprintf(out, size, "模拟器不扫描输入设备");
}

#else

#include <linux/input.h>

static lv_indev_t *touch;
static lv_display_t *target_display;
static char device_path[80];
static unsigned long long last_try_ms;

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

/* 内核的 test_bit 在用户态没有，自己判一位 */
static bool bit_set(const unsigned long *bits, int bit)
{
    const int per = (int)(8 * sizeof(unsigned long));
    return (bits[bit / per] >> (bit % per)) & 1UL;
}

/* 这个 event 设备像不像触摸屏：有绝对坐标轴（单点 ABS_X/Y 或多点 ABS_MT_*） */
static bool looks_like_touch(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;

    unsigned long bits[(ABS_MAX / (8 * (int)sizeof(unsigned long))) + 2];
    memset(bits, 0, sizeof(bits));
    bool ok = false;
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) >= 0) {
        if (bit_set(bits, ABS_X) && bit_set(bits, ABS_Y)) ok = true;
        if (bit_set(bits, ABS_MT_POSITION_X) && bit_set(bits, ABS_MT_POSITION_Y)) ok = true;
    }
    close(fd);
    return ok;
}

static const char *find_touch(char *out, size_t size)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) return NULL;
    const char *found = NULL;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[96];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (looks_like_touch(path)) {
            snprintf(out, size, "%s", path);
            found = out;
            break;
        }
    }
    closedir(dir);
    return found;
}

void qzcam_touch_attach(lv_display_t *display)
{
    if (display) target_display = display;
    if (touch) return;

    last_try_ms = now_ms();

    /* 显式指定优先（调试用），否则自动找 */
    const char *want = getenv("QZCAM_TOUCH");
    if (want && want[0] && strcmp(want, "auto") != 0) {
        snprintf(device_path, sizeof(device_path), "%s", want);
    } else {
        device_path[0] = '\0';
        if (!find_touch(device_path, sizeof(device_path))) {
            fprintf(stderr,
                    "QZcam: 暂时没找到触摸屏（/dev/input/event* 里没有带绝对坐标轴的），稍后自动重试\n");
            return;
        }
    }

    touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, device_path);
    if (!touch) {
        fprintf(stderr, "QZcam: 打不开触摸节点 %s\n", device_path);
        device_path[0] = '\0';
        return;
    }
    if (target_display) lv_indev_set_display(touch, target_display);
    fprintf(stderr, "QZcam: 触摸已就绪 %s\n", device_path);
}

void qzcam_touch_poll(void)
{
    if (touch) return;
    unsigned long long now = now_ms();
    if (now - last_try_ms < 2000) return;   /* 2 秒一次就够，别在热路径上扫目录 */
    qzcam_touch_attach(NULL);
}

const char *qzcam_touch_device(void)
{
    return device_path[0] ? device_path : "(未找到)";
}

bool qzcam_touch_ready(void)
{
    return touch != NULL;
}

void qzcam_touch_scan(char *out, unsigned size)
{
    out[0] = '\0';
    size_t used = 0;
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        snprintf(out, size, "(没有 /dev/input 目录)");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[96];
        char name[64] = "?";
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            close(fd);
        }
        if (used + 1 < size) {
            int written = snprintf(out + used, size - used, "%s%s%s=%s",
                                   used ? " " : "", path,
                                   looks_like_touch(path) ? "[触摸]" : "", name);
            if (written > 0) used += (size_t)written;
        }
    }
    closedir(dir);
    if (used == 0) snprintf(out, size, "(没有 /dev/input/event*)");
}

#endif /* LV_USE_SIMULATOR */
