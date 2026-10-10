/* cam_player —— 板载录像回放
 *
 * 按文件头自动分后端：
 *   AVI（未压缩 RGB565）：解析头+idx1 索引，逐帧读块 + 翻行，零解码。
 *   MP4（H.264/H.265）：rkdemuxer 拆包 → MPI VDEC 解码 → 缩到面板尺寸 RGB565。
 *   没有媒体库的构建里 MP4 打不开（open 返回 false），AVI 照常。
 *
 * 用法：
 *   qzcam_player_open("/mnt/sdcard/VID_xxx.mp4")
 *   qzcam_player_next_frame()                       // 逐帧（面板尺寸 RGB565）
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

/** 最近一次 open 失败的原因（给界面显示用）；空串 = 没有额外说明 */
const char *qzcam_player_last_error(void);

#endif
