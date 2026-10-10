#ifndef QZCAM_RECORD_H
#define QZCAM_RECORD_H

#include <stdbool.h>
#include <stdint.h>

/* 录像：把取景帧落成文件。两条路自动选：
 *
 *   MP4（首选，真机有媒体库且 ISP 起流时）——VI 的 NV12 直接喂 VENC 硬编码
 *     H.264/H.265，rkmuxer 封 MP4。编码在硬件里做，CPU 不参与，文件小一到两个
 *     数量级；帧从 qzcam_rec_submit_isp() 进来。
 *   AVI（兜底，模拟器 / 没起 ISP / 硬件编码不可用）——未压缩 RGB565，零依赖、
 *     当场可播，代价是文件大（320x240 约 150KB/帧）。延时摄影也走这条（按间隔
 *     送帧，没有连续 NV12 可喂）；帧从 qzcam_rec_submit() 进来。
 *
 * 线程模型：submit/submit_isp 由取景线程每帧调用（内部按 settings->fps 抽帧）；
 * start/stop 由界面线程调用，两边用锁交接；两条路都支持循环分段与掉电恢复。 */

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

/** 非人为停止的原因（写失败/空间不足），没发生过自动停止时是空串。 */
const char *qzcam_rec_stop_reason(void);

/** 送一帧（面板尺寸 RGB565）。不录时是空操作。 */
void qzcam_rec_submit(const uint8_t *rgb565, int w, int h);

/** 当前设置（分辨率 + 录制画质）下的录像码率上限估计，单位 kbps。
 * 界面拿它折算"还能录多少分钟"；实际是 AVBR，静止画面会明显低于这个上限。 */
int qzcam_rec_bitrate_kbps(void);

/**
 * 送一帧 ISP 的 NV12（录像走硬件编码时用这条）。
 *
 * video_frame 传 qzcam_isp_held_frame() 的返回值（VIDEO_FRAME_INFO_S*，不透明），
 * w/h 是画面尺寸，stride/vir_h 是 VI 缓冲的行宽与行数。这条路只有 ISP 后端有
 * NV12 可给；没有编码器时它是空操作，录像还是靠 submit() 走未压缩 AVI。
 */
void qzcam_rec_submit_isp(void *video_frame, int w, int h, int stride, int vir_h);

/** 开始延时摄影：帧由界面按间隔送入，视频按 out_fps 播放。已在录时返回 false。 */
bool qzcam_rec_tl_start(char *path, unsigned path_size, int out_fps);

/** 延时摄影送帧：不做抽帧节流，来一帧写一帧。 */
void qzcam_rec_submit_now(const uint8_t *rgb565, int w, int h);

/**
 * 异常录像恢复：把存储目录里「掉电没来得及收尾」的 AVI 补回索引。
 *
 * 录像中断电时，文件头里的 RIFF/movi 长度、avih/strh 帧数都还是 0，也还没写
 * idx1 —— 这种文件播放器基本认不出来（或只能认出 0 帧）。这里重新扫一遍 movi
 * 里的 `00dc` 帧块，据此补写 idx1、回填各部分长度与帧数，并把掉电时写了一半的
 * 尾帧截掉。返回修好的文件数（没坏的就是 0）。
 *
 * 开机调一次；相册每次打开也调一次（插卡晚了也能被补上）。正在录的那个文件会
 * 被跳过，不会边写边改。
 */
int qzcam_rec_repair_all(void);

#endif
