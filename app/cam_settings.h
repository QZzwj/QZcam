#ifndef QZCAM_CAM_SETTINGS_H
#define QZCAM_CAM_SETTINGS_H

#include <stdbool.h>

/* 参数与叠加的状态都在 include/qzcam.h 的 qzcam_settings_t 里，这里只补两件
 * 界面需要的小工具：存图目录解析、参数复位。 */

/** 快门存图目录：QZCAM_DIR → /userdata/qzcam → /tmp/qzcam（返回的缓冲是静态的）。 */
const char *qzcam_settings_photo_dir(void);

/** 参数复位到默认值。 */
void qzcam_settings_reset(void);

#endif
