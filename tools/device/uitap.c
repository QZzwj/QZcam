/* uitap —— QZcam 虚拟触摸屏守护
 *
 * 读 /tmp/qz-touch.fifo 里的命令，通过 /dev/uinput 注入一颗虚拟触摸屏
 * （"qz-remote-touch"），qzcam 会像摸真屏一样收到它。用途：
 *   - 电脑上的远程桌面（QZcam/tools/remote_desktop.py）把鼠标事件写进来；
 *   - 任何自动化/演示：adb shell "echo 'tap 160 217' > /tmp/qz-touch.fifo"
 *
 * 命令格式（一行一条）：
 *   tap X Y
 *   swipe X1 Y1 X2 Y2 MS
 *
 * 面板尺寸开机从 fb0 的 virtual_size 读（读不到按 320x240）。
 * 由 S30qzcam 开机拉起；手动跑也行：/oem/usr/bin/uitap &
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <linux/uinput.h>
#include <linux/input.h>

#define FIFO_PATH "/tmp/qz-touch.fifo"

static int ufd = -1;
static int panel_w = 320, panel_h = 240;

static void emit(int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(ufd, &ev, sizeof(ev)) < 0)
        perror("uitap: uinput write");
}

static void syn(void)
{
    emit(EV_SYN, SYN_REPORT, 0);
}

static void touch_down(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= panel_w) x = panel_w - 1;
    if (y >= panel_h) y = panel_h - 1;
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    emit(EV_KEY, BTN_TOUCH, 1);
    syn();
}

static void touch_move(int x, int y)
{
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    syn();
}

static void touch_up(void)
{
    emit(EV_KEY, BTN_TOUCH, 0);
    syn();
}

static void do_tap(int x, int y)
{
    touch_down(x, y);
    usleep(30000);          /* 按住 30ms，LVGL 的按下/释放都能吃到 */
    touch_up();
}

static void do_swipe(int x1, int y1, int x2, int y2, int ms)
{
    if (ms < 50) ms = 50;
    if (ms > 3000) ms = 3000;
    int steps = ms / 10;
    touch_down(x1, y1);
    for (int i = 1; i <= steps; i++) {
        touch_move(x1 + (x2 - x1) * i / steps,
                   y1 + (y2 - y1) * i / steps);
        usleep(10000);      /* 10ms/步，平滑轨迹 */
    }
    touch_up();
}

static int uinput_init(void)
{
    ufd = open("/dev/uinput", O_WRONLY);
    if (ufd < 0) {
        fprintf(stderr, "uitap: 打不开 /dev/uinput: %s\n", strerror(errno));
        return -1;
    }
    ioctl(ufd, UI_SET_EVBIT, EV_SYN);
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    ioctl(ufd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(ufd, UI_SET_EVBIT, EV_ABS);
    ioctl(ufd, UI_SET_ABSBIT, ABS_X);
    ioctl(ufd, UI_SET_ABSBIT, ABS_Y);

    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "qz-remote-touch");
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.version = 1;
    dev.absmin[ABS_X] = 0;
    dev.absmax[ABS_X] = panel_w - 1;
    dev.absmin[ABS_Y] = 0;
    dev.absmax[ABS_Y] = panel_h - 1;
    if (write(ufd, &dev, sizeof(dev)) < 0 ||
        ioctl(ufd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "uitap: 创建 uinput 设备失败: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void panel_size_init(void)
{
    FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (f) {
        int w = 0, h = 0;
        if (fscanf(f, "%d,%d", &w, &h) == 2 && w > 0 && h > 0) {
            panel_w = w;
            panel_h = h;
        }
        fclose(f);
    }
}

int main(void)
{
    panel_size_init();
    if (uinput_init() < 0)
        return 1;
    /* 旧 fifo 残留（异常退出没清）也无妨：同名 mkfifo 会 EEXIST，直接用 */
    if (mkfifo(FIFO_PATH, 0666) < 0 && errno != EEXIST) {
        fprintf(stderr, "uitap: mkfifo: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "uitap: 就绪（%dx%d，写 %s 即可注入触摸）\n",
            panel_w, panel_h, FIFO_PATH);

    for (;;) {
        /* 读端用完即关再重开：写端（adb shell）随时来随时有 */
        int ffd = open(FIFO_PATH, O_RDONLY);
        if (ffd < 0) {
            sleep(1);
            continue;
        }
        FILE *fp = fdopen(ffd, "r");
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            int a, b, c, d, e;
            if (sscanf(line, "tap %d %d", &a, &b) == 2) {
                do_tap(a, b);
            } else if (sscanf(line, "swipe %d %d %d %d %d",
                              &a, &b, &c, &d, &e) == 5) {
                do_swipe(a, b, c, d, e);
            } else if (sscanf(line, "swipe %d %d %d %d", &a, &b, &c, &d) == 4) {
                do_swipe(a, b, c, d, 250);
            }
        }
        fclose(fp);         /* 写端全关了（EOF），重开等下一轮 */
    }
    return 0;
}
