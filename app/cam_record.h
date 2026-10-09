#ifndef QZCAM_RECORD_H
#define QZCAM_RECORD_H

#include <stdbool.h>
#include <stdint.h>

/* 录像：把取景帧写成一个能直接播放的 AVI 文件。
 *
 * 为什么是 AVI + 未压缩 RGB565：交叉 sysroot 里没有 libjpeg（编不了 MJPEG），
 * 硬件 H.264（MPP/VENC）要另外接一大套 MPI。这条路**零依赖**、当场可播，
 * 代价是文件大（320x240 约 150KB/帧；15fps ≈ 2.2MB/s）。升级路径写在这里：
 * 把 qzcam_rec_submit() 里的落盘换成 VENC 编码 + MP4 封装即可，界面不用动。
 *
 * 线程模型：submit 由取景线程每帧调用（内部按 settings->fps 抽帧）；
 * start/stop 由界面线程调用，两边用锁交接。 */

/** 开始录像；path 收下实际文件名（存图目录由 cam_storage 决定）。已在录时返回 false。 */
bool qzcam_rec_start(char *path, unsigned path_size);

/** 停止录像并写索引/修补文件头。没在录时是空操作。 */
void qzcam_rec_stop(void);

bool qzcam_rec_active(void);

/** 已录秒数（界面上的计时）。 */
int qzcam_rec_seconds(void);

/** 已录帧数。 */
int qzcam_rec_frames(void);

/** 已写入字节数。 */
unsigned long long qzcam_rec_bytes(void);

/** 送一帧（面板尺寸 RGB565）。不录时是空操作。 */
void qzcam_rec_submit(const uint8_t *rgb565, int w, int h);

#endif
