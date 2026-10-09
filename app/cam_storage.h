#ifndef QZCAM_STORAGE_H
#define QZCAM_STORAGE_H

#include <stdbool.h>

/* 运动相机的落地存储：拍照与录像都写这里。
 *
 * 选择顺序（第一个可写的胜出）：
 *   QZCAM_DIR 环境变量（测试用）→ /mnt/sdcard（SD 卡，运动相机首选）
 *   → /userdata/qzcam（板载用户区）→ /tmp/qzcam（最后兜底，重启就没了）
 *
 * free_bytes/percent 供界面顶栏显示剩余空间；低于 10% 时界面会提示。 */

/** 可写目录（内部缓存，第一次调用时探测；用 qzcam_storage_refresh() 重新探测）。 */
const char *qzcam_storage_dir(void);

/** 存储介质的中文短名：`SD 卡` / `用户区` / `临时`。 */
const char *qzcam_storage_label(void);

unsigned long long qzcam_storage_free_bytes(void);

/** 剩余百分比（0..100）；拿不到时返回 100。 */
int qzcam_storage_free_percent(void);

/** 目录可用吗（可写）。 */
bool qzcam_storage_ready(void);

/** 重新探测（插拔 SD 卡后调用）。 */
void qzcam_storage_refresh(void);

#endif
