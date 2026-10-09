#ifndef QZCAM_FONT_H
#define QZCAM_FONT_H

#include "lvgl.h"

/* 中文界面字体：运行时读 TTF（LV_USE_TINY_TTF），按需生成字号。
 * 板上字体没装时退回 ASCII 字体，界面依然可用（中文会变方框）。 */

void qzcam_font_init(void);

/** 正文字体（含中文）。px 为期望像素高度。 */
const lv_font_t *qzcam_font(int px);

/** 图标字体（LV_SYMBOL_* 那套符号，中文 TTF 里没有这些字形）。 */
const lv_font_t *qzcam_icon(int px);

#endif
