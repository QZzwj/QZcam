#ifndef QZCAM_CAM_ISP_H
#define QZCAM_CAM_ISP_H

#include <stdbool.h>
#include <stdint.h>

/**
 * 真机取景后端：Rockchip 官方 ISP 管线（rkaiq 3A + RKMPI VI 出 NV12）。
 *
 * 这条路与 Echo-Mate / SDK 官方示例（rkisp_demo、luckfox_pico_rtsp_*）一致：
 *
 *   rk_aiq_uapi2_sysctl_init/prepare/start   3A + IQ 文件 → 出彩色图
 *     → RK_MPI_SYS_Init + VI Channel(NV12)   取流走 rockit
 *     → RK_MPI_VI_GetChnFrame / ReleaseChnFrame
 *
 * 为什么必须走它：裸 V4L2 直取 rkisp_mainpath 绕过了 3A（AE/AWB），拿到的图
 * 偏暗/偏绿，甚至因为 ISP 没被配置起来而一帧都不出 —— 这是上一版"没真图"的
 * 根因。所以 cam_core 优先用它，V4L2 只当兜底。
 *
 * 板上依赖（都由镜像带过去，见 SDK/project/app/qzcam/Makefile）：
 *   librkaiq.so / librockit.so / librockchip_mpp.so  （/oem/usr/lib）
 *   rockit.ko                                       （S30qzcam 最后 insmod）
 *   IQ 文件：默认 /etc/iqfiles（sc3336_CMK-OT2119-PC1_30IRC-F16.json/.bin）
 *
 * 模拟器构建（QZCAM_SIMULATOR）里全是桩：open() 直接返回 false，界面自动落到
 * 别的后端，开发机照样能跑 UI 与存图链路。
 */

/** 初始化 rkaiq + MPI 并开 VI 流。已在跑时直接返回 true。失败原因见 last_error。 */
bool qzcam_isp_open(int width, int height);

/**
 * 取一帧 NV12。
 * *width/*height 返回帧尺寸，*stride 返回行宽（u32VirWidth）；Y 面 height 行，
 * UV 面紧跟在 Y 面之后（height/2 行，交织）。取到的帧必须用 release 归还。
 * 超时/失败返回 NULL。
 */
const uint8_t *qzcam_isp_grab(int *width, int *height, int *stride, int timeout_ms);

/** 归还上一帧（每次 grab 之后、下次 grab 之前调用）。 */
void qzcam_isp_release(void);

/** 关流 + 反初始化（可重复调用）。 */
void qzcam_isp_close(void);

/** 最近一次失败原因（一行，可直接显示在界面上）。 */
const char *qzcam_isp_last_error(void);

#endif
