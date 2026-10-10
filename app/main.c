#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cam_core.h"
#include "cam_record.h"
#include "cam_ui.h"
#include "cam_ui.h"
#include "font.h"
#include "lvgl.h"
#include "qzcam.h"
#include "touch.h"

/* QZcam：开机直接进相机的独立程序。
 *
 * 与 QZdesk 的桌面程序共用一套底座（LVGL + fbdev/evdev + SDL 模拟器），但只有
 * 一个界面：全屏取景。开机流程里由 S30qzcam 拉起，退出即黑屏 —— 没有桌面、
 * 没有多任务。 */

static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : fallback;
}

int main(void)
{
    /* 面板尺寸要先定下来：字体、布局、取景缓冲都按它算。 */
    qzcam_panel_init();
    qzcam_settings_load();   /* 上次的参数（拍摄模式/EV/白平衡等）先读回来 */

    lv_init();
    qzcam_font_init();

#if LV_USE_SIMULATOR
    lv_sdl_window_create(qzcam_panel_w, qzcam_panel_h);
    lv_sdl_mouse_create();
#else
    lv_display_t *display = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(display, env_or("QZCAM_FB", "/dev/fb0"));
    /* 触摸：自动认触摸屏（按绝对坐标轴的能力找），不再是写死的 event0 */
    qzcam_touch_attach(display);
#endif

    /* 先起取景再建界面：界面一出来就有画面，不会先闪一屏黑。 */
    if (!qzcam_cam_start()) {
        fprintf(stderr, "QZcam: 取景启动失败：%s\n", qzcam_cam_status());
    }

    /* 掉电保护：把上次没收尾的录像文件补回索引（相册每次打开还会再补一次，
     * 覆盖「开机后才插卡」的情况）。 */
    int repaired = qzcam_rec_repair_all();
    if (repaired > 0) fprintf(stderr, "QZcam: 恢复 %d 个掉电录像文件\n", repaired);

    lv_obj_t *screen = qzcam_ui_create();
    lv_screen_load(screen);
    lv_refr_now(NULL);

    fprintf(stderr, "QZcam: 后端=%s 状态=%s 触摸=%s\n", qzcam_cam_backend(),
            qzcam_cam_status(), qzcam_touch_device());

    /* 自检钩子：QZCAM_SNAP_AT=秒 表示"N 秒后自动拍一张"，用来在开发机/CI 上
     * 验证「取景 → 转码 → 参数 → 存图」整条链路（没有触摸屏可点）。 */
    /* 自检钩子：QZCAM_TEST_REC=秒 —— 1 秒后开始录像，录满 N 秒停止并打印结果，
     * 用来在开发机上验证「取景 → 录像落盘」这条链路（AVI 可直接用 ffprobe 校验）。 */
    /* 注意：这几个时刻都要相对「主循环第一次跑」来算，不能用 lv_tick 从 0 起的
     * 绝对值 —— 真机上 ISP + rkaiq 3A 初始化要 9 秒以上，等主循环开始时 tick 早就
     * 越过 1 秒了，钩子会在同一次循环里把"开始"和"停止"背靠背跑完（录到 0 帧）。 */
    const char *rec_env = getenv("QZCAM_TEST_REC");
    double rec_seconds = rec_env ? atof(rec_env) : 0.0;
    double loop_origin = (double)lv_tick_get() / 1000.0;
    double rec_start_at = loop_origin + 1.0;
    double rec_stop_at = rec_start_at + rec_seconds;
    bool rec_started = false, rec_done = false;

    /* 自检钩子：QZCAM_TEST_GALLERY=1 打开相册；QZCAM_TEST_VIEWER=1 再看最新那张照片；
     * QZCAM_TEST_PLAY=1 直接回放最新一段录像（MP4 走 VDEC，没法只看代码确认）。 */
    bool test_gallery = getenv("QZCAM_TEST_GALLERY") != NULL;
    bool test_viewer = getenv("QZCAM_TEST_VIEWER") != NULL;
    bool test_play = getenv("QZCAM_TEST_PLAY") != NULL;
    bool gallery_done = false, viewer_done = false, play_done = false;

    const char *snap_env = getenv("QZCAM_SNAP_AT");
    double snap_at = snap_env ? atof(snap_env) : 0.0;
    double started = (double)lv_tick_get() / 1000.0;
    bool snapped = false;

    for (;;) {
        uint32_t idle = lv_timer_handler();
        if (idle < 4) idle = 4;
        if (idle > 30) idle = 30;
        usleep(idle * 1000);

        /* 触摸屏晚注册（或热插拔）时自动补上，界面不至于"按钮点了没反应" */
        qzcam_touch_poll();

        if ((test_gallery || test_viewer) && !gallery_done) {
            if ((double)lv_tick_get() / 1000.0 >= loop_origin + 0.3) {
                gallery_done = true;
                qzcam_ui_open_gallery();
            }
        }
        if (test_play && !play_done) {
            if ((double)lv_tick_get() / 1000.0 >= loop_origin + 1.0) {
                play_done = true;
                qzcam_ui_play_first_video();
            }
        }
        if (test_viewer && gallery_done && !viewer_done) {
            if ((double)lv_tick_get() / 1000.0 >= loop_origin + 0.9) {
                viewer_done = true;
                qzcam_ui_view_first_photo();
            }
        }

        if (rec_seconds > 0.0) {
            double now = (double)lv_tick_get() / 1000.0;
            if (!rec_started && now >= rec_start_at) {
                char path[240];
                rec_started = true;
                qzcam_ui_set_mode(1);      /* 界面切到录像模式（红按钮） */
                if (qzcam_rec_start(path, sizeof(path)))
                    fprintf(stderr, "QZcam: 自检录像开始 -> %s\n", path);
                else
                    fprintf(stderr, "QZcam: 自检录像启动失败\n");
            } else if (rec_started && !rec_done && now >= rec_stop_at) {
                rec_done = true;
                qzcam_rec_stop();
                qzcam_ui_set_mode(0);      /* 回到拍照模式 */
                fprintf(stderr, "QZcam: 自检录像结束 帧数=%d 字节=%llu\n",
                        qzcam_rec_frames(), qzcam_rec_bytes());
            }
        }

        if (snap_at > 0.0 && !snapped) {
            double now = (double)lv_tick_get() / 1000.0;
            if (now - started >= snap_at) {
                snapped = true;
                char path[240];
                if (qzcam_cam_snapshot(path, sizeof(path)))
                    fprintf(stderr, "QZcam: 自检快门 -> %s\n", path);
                else
                    fprintf(stderr, "QZcam: 自检快门失败：%s\n", qzcam_cam_status());
            }
        }
    }

    qzcam_cam_stop();
    return 0;
}
