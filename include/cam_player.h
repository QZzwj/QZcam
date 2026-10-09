/* cam_player —— 板载录像（AVI 未压缩 RGB565）回放解析器
 *
 * 用法：
 *   qzcam_player_open("/mnt/sdcard/VID_xxx.avi")   // 解析头+索引
 *   qzcam_player_next_frame()                       // 逐帧（自上而下 RGB565）
 *   qzcam_player_seek_frame(n) / total / fps / w / h
 *   qzcam_player_close()
 */
#ifndef QZCAM_PLAYER_H
#define QZCAM_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

bool qzcam_player_open(const char *path);
const uint8_t *qzcam_player_next_frame(void);   /* NULL = 播完/出错 */
void qzcam_player_seek_frame(int n);
int qzcam_player_frame_index(void);
int qzcam_player_total_frames(void);
int qzcam_player_frame_w(void);
int qzcam_player_frame_h(void);
int qzcam_player_fps(void);
void qzcam_player_close(void);

#endif
