#ifndef QZCAM_UI_H
#define QZCAM_UI_H

#include "lvgl.h"

/* 相机界面：全屏取景 + 顶栏（状态/时间/FPS）+ 底部按钮（参数/快门）+ 参数抽屉。
 * 取景画面每帧从 cam_core 取；叠加（网格/时间戳）按 qzcam_settings() 显示。 */

lv_obj_t *qzcam_ui_create(void);

/** 切换拍摄模式：0=拍照，1=录像（界面按钮与主按钮配色随动）。 */
void qzcam_ui_set_mode(int video);

/* 自检用入口（供 QZCAM_TEST_* 环境变量驱动，正常用户操作走按钮） */
void qzcam_ui_open_gallery(void);        /**< 打开相册列表 */
void qzcam_ui_view_first_photo(void);    /**< 全屏查看最新一张照片 */
void qzcam_ui_play_first_video(void);    /**< 直接回放最新一段录像（自检用） */

#endif
