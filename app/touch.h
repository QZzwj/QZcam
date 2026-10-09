#ifndef QZCAM_TOUCH_H
#define QZCAM_TOUCH_H

#include <stdbool.h>

#include "lvgl.h"

/**
 * 触摸输入：**自动认出触摸屏**，而不是死用 /dev/input/event0。
 *
 * 为什么要自动认：event 编号由**驱动注册顺序**决定 —— 按键、电源键、ADC 键、
 * 红外、甚至 USB 输入设备都可能排在触摸前面。板子上换个固件/多一个驱动，写死的
 * /dev/input/event0 就点不动了（界面在、按钮没反应）。触摸屏的特征是"有绝对坐标
 * 轴"（ABS_X/ABS_Y 或多点 ABS_MT_POSITION_X/Y），所以按能力找才可靠。
 *
 * QZCAM_TOUCH 可以强制指定节点（如 /dev/input/event1）；留空或写 auto 就是自动找。
 *
 * 本板子的触控是 FocalTech FTS（DTB 里 focaltech@38，内核 CONFIG_TOUCHSCREEN_FTS=y
 * 内建，不需要 insmod）。
 */

/** 绑定显示并尝试建立触摸输入（内部记住 display，供 poll 复用）。 */
void qzcam_touch_attach(lv_display_t *display);

/** 周期性调用（跟着主循环走就行）：还没就绪就重试 —— 驱动晚注册也能自愈。 */
void qzcam_touch_poll(void);

/** 当前用上的（或正在等的）触摸节点；日志用。 */
const char *qzcam_touch_device(void);

/** 触摸是否已就绪。 */
bool qzcam_touch_ready(void);

/** 把 /dev/input/event* 扫成一行（哪个是触摸、叫什么名字），自检报告用。 */
void qzcam_touch_scan(char *out, unsigned size);

#endif
