#ifndef QZCAM_ENCODER_H
#define QZCAM_ENCODER_H

#include <stdbool.h>

/* 硬编码压缩录像的编码侧：VI 的 NV12 帧 → VENC(H.264/H.265) → rkmuxer 封 MP4。
 *
 * 为什么值得单独一层：RGB565 的 AVI 是「拿多大的画面就写多大的字节」，一帧
 * 320x240 就是 150KB；H.264/H.265 靠帧间预测能压一到两个数量级，而且编码是
 * RV1106 的硬件单元在做（VENC），CPU 只是把 VI 的 NV12 指针递过去 —— 连
 * NV12→RGB565 那步转码都省了。
 *
 * 线程模型：
 *   送帧线程   qzcam_enc_send()      取景线程调用，把 VI 的帧丢给 VENC
 *   取流线程   内部                VENC 出码流 → rkmuxer 写 MP4
 *   开/关      open()/close()       界面线程（录像开始/停止）
 *
 * 没有 Rockchip 媒体库（模拟器构建）时整层是桩：available() 恒 false，
 * 录像自动落回 AVI。 */

/** 这条路可用吗：媒体库在、且 ISP 已经起流（有 NV12 帧可喂）。 */
bool qzcam_enc_available(void);

/**
 * 开一路 MP4。w/h 是画面尺寸，vir_w/vir_h 是 VI 缓冲的行宽/行数（来自
 * qzcam_isp_grab 的 stride 与 qzcam_isp_virtual_height），必须与实际帧一致，
 * 否则编码器按错的 stride 取 UV 面会偏色甚至花屏。
 * codec：0=H.265 1=H.264；rc_mode：0=智能编码 1=CBR 2=VBR。
 * 已在编码时返回 false。
 */
bool qzcam_enc_open(const char *path, int w, int h, int vir_w, int vir_h,
                    int fps, int codec, int rc_mode, int bitrate_kbps);

/** 送一帧。video_frame 就是 qzcam_isp_held_frame() 返回的那个 VIDEO_FRAME_INFO_S*。
 * 返回 false 表示这一帧没送出去（编码通道没了或写盘出错，调用方应收尾）。 */
bool qzcam_enc_send(void *video_frame);

/** 收尾：落 moov、销毁编码通道。可重复调用；没在编码时是空操作。 */
void qzcam_enc_close(void);

bool qzcam_enc_active(void);

int qzcam_enc_frames(void);
unsigned long long qzcam_enc_bytes(void);

/** 最近一次失败原因（已封装在中文短句里），正常时空串。 */
const char *qzcam_enc_last_error(void);

#endif
