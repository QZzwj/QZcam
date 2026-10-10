#ifndef QZCAM_CAM_CORE_H
#define QZCAM_CAM_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* 取景引擎：把摄像头的一帧变成「面板尺寸的 RGB565」交给界面。
 *
 * 两个后端：
 *   1. V4L2（真机）：rkisp 会注册一堆 /dev/videoN，能出彩色预览的是
 *      rkisp_mainpath —— 节点编号不固定（官方文档里在 /dev/video15 一带，还有
 *      USB 摄像头占 /dev/video0 的情况），所以按 sysfs 的
 *      /sys/class/video4linux/videoN/name 认名字。优先 NV12，退回 YUYV；
 *      采集尺寸按 640x480 → 1280x720 → 2304x1296（mainpath 原生）逐档试。
 *
 *      注意：官方 CSI 文档明确写着"未经 ISP 处理的 V4L2 采集会偏暗、偏绿，
 *      要 ISP 处理过的图请用 RKMPI 示例或默认的 RKIPC"。这条路目前是 V4L2
 *      直取，真机上若发现偏色，就是缺 rkaiq 3A 那一层（下一步接 rkaiq）。
 *      /dev/video* 同一时刻只能被一个进程打开，所以启动前要先停掉 rkipc
 *      （见 S30qzcam 的 stop_camera_hogs）。
 *   2. 测试后端（QZCAM_BACKEND=fake）：生成会动的彩条。**只有显式指定才会用**，
 *      开发机/CI 上把「取景 → 上屏 → 快门存图」整条链路跑通用的。
 *
 * 默认行为是"只认真机"：真机打不开时**不会**退回彩条（假画面会让人以为相机在
 * 工作），而是每 2 秒自动重连，界面上盖一层"相机无信号 + 原因"（见
 * qzcam_cam_healthy() / qzcam_cam_diag()）—— 没串口也能一眼看出卡在哪一步。
 * 出流后若连续 3 秒没有帧，也按"断流"处理并自动重连。
 *
 * 帧的交接走三缓冲：工作线程转换满一帧就发布（递增 frame_seq），界面线程用
 * qzcam_cam_frame() 取最新那帧；界面渲染期占着的那块缓冲工作线程会避让。
 *
 * 环境变量：
 *   QZCAM_BACKEND=fake|v4l2   强制后端（默认走真机 V4L2；fake 只在自检时用）
 *   QZCAM_DEV=/dev/videoN     指定取景节点（跳过名字探测）
 *   QZCAM_CAPTURE=640x480     向驱动请求的采集尺寸
 *   QZCAM_DIR=/path           快门存图目录（不设则 SD 卡优先：/mnt/sdcard →
 *                             /userdata/qzcam → /tmp/qzcam，周期性重探支持热插拔）
 */

/** 启动取景。已在跑时返回 false；失败原因看 qzcam_cam_status()。 */
bool qzcam_cam_start(void);

/** 停止取景并释放摄像头。没在跑时是空操作。 */
void qzcam_cam_stop(void);

/** 正在运行吗。 */
bool qzcam_cam_running(void);

/**
 * 真机画面可用吗 —— 真机流**正在出帧**才算（"打开了节点"不算）。
 * 界面用它决定要不要盖"无信号 + 原因"的浮层；彩条自检后端按可用算。
 */
bool qzcam_cam_healthy(void);

/** 真机起不来时的现场信息（多行，直接可显示）；可用时是空串。 */
const char *qzcam_cam_diag(void);

/** 后端描述（`V4L2 /dev/video11 640x480 NV12`、`测试后端（彩条）`、`未启动`）。 */
const char *qzcam_cam_backend(void);

/** 一行状态：为什么不可用，或当前跑到哪一步。 */
const char *qzcam_cam_status(void);

/** 帧尺寸（面板尺寸）。 */
int qzcam_cam_frame_w(void);
int qzcam_cam_frame_h(void);

/**
 * 取最新一帧（RGB565，面板尺寸）。
 *
 * `*id` 返回帧序号，和上次一样表示没有新内容。返回的缓冲在下一次取之前不会被
 * 改写，可以直接交给 lv_image 渲染。还没有任何帧时返回 NULL。
 */
const uint8_t *qzcam_cam_frame(uint32_t *id);

/** 实测取景帧率（每秒刷新一次，平滑过）。 */
double qzcam_cam_fps(void);

/** 本次启动以来拍下的照片数。 */
int qzcam_cam_photos(void);

/** Capture the next native frame, enhance and save JPEG; call from a task thread. */
bool qzcam_cam_snapshot(char *path, unsigned path_size);

#endif
