#ifndef QZCAM_H
#define QZCAM_H

#include <stdbool.h>
#include <stdint.h>

/* QZcam：相机应用（独立固件，开机直接进相机）。
 *
 * 这个头文件只放「面板尺寸」和「运行参数」两件全局状态，其余能力各自成模块：
 *   cam_core.c      取景引擎（V4L2 / 测试后端 → 面板尺寸 RGB565）
 *   cam_settings.c  参数（亮度/对比/镜像/翻转）与叠加开关的状态与实现
 *   cam_ui.c        界面（全屏预览 + 顶栏 + 底部按钮 + 参数抽屉）
 *   panel.c         面板尺寸探测（真机 /dev/fb0，模拟器 QZCAM_PANEL）
 *   font.c          中文字体（tiny_ttf 读 TTF，缺字体时退回 ASCII）
 */

/** 面板实际尺寸（像素）。界面的一切布局都按它算。 */
extern int qzcam_panel_w;
extern int qzcam_panel_h;

/** 探测面板尺寸：模拟器读 QZCAM_PANEL（默认 320x240），真机读 QZCAM_FB（默认 /dev/fb0）。 */
void qzcam_panel_init(void);

/** 运行参数。界面上的参数抽屉改的就是这个结构，取景线程每帧读它。 */
typedef struct {
    /* ---- 叠加与画面（逐像素生效，立即见效） ---- */
    bool grid;      /**< 九宫格参考线 */
    bool timestamp; /**< 画面右上角时间戳 */
    bool mirror;    /**< 左右镜像 */
    bool flip;      /**< 上下翻转（装配方向不同时的兜底） */
    int brightness; /**< -50..50 */
    int contrast;   /**< 50..150，100 = 原始对比 */
    int fps;        /**< 取景帧率上限，5..30 */

    /* ---- 模式与录制（SC3336 上限 2304x1296@30fps） ---- */
    int mode;          /**< 0=视频 1=照片 2=慢动作 3=延时 4=循环 */
    int resolution;    /**< 0=2304x1296 1=1920x1080 2=1280x720 */
    int rec_fps;       /**< 30 / 25 / 15 */
    int codec;         /**< 0=H.265 1=H.264 */
    int bitrate_mode;  /**< 0=智能编码 1=CBR 2=VBR */
    bool eis;          /**< 电子防抖 */
    int loop_seconds;  /**< 循环分段：60/180/300/600 */
    bool loop_overwrite; /**< 循环覆盖 */

    /* ---- 图像（ISP3.2） ---- */
    bool hdr;          /**< HDR */
    bool wdr;          /**< 宽动态 */
    int isp_denoise;   /**< 0=关 1=低 2=中 3=高（3DNR/2DNR） */
    int wb_mode;       /**< 0=自动 1=日光 2=阴天 3=白炽灯 4=手动 */
    int ev;            /**< -30..30 = -3.0..+3.0 EV（0.1 步） */
    bool night_mode;   /**< 夜视增强（PixGain 双增益） */

    /* ---- 参数面板（底部弹出，实时生效） ---- */
    int wb_kelvin;     /**< 2500..8000 K（wb_mode=手动时生效） */
    int sharpness;     /**< 0..100 */
    int denoise;       /**< 0..100 */

    /* ---- 音频 ---- */
    bool audio_rec;      /**< 录音（内置 Audio Codec） */
    bool audio_denoise;  /**< AI 降噪 */

    /* ---- 通用 ---- */
    bool watermark;    /**< 日期水印 */
    int auto_off;      /**< 自动关机：0=从不，否则秒（60/180/300） */
} qzcam_settings_t;

/** 全局参数（带默认值，见 cam_settings.c）。 */
qzcam_settings_t *qzcam_settings(void);

/**
 * 从磁盘读参数（key=value 文本）。开机时调一次；文件不存在就用默认值。
 * 位置依次尝试 /oem/usr/etc → /userdata → /tmp。
 */
void qzcam_settings_load(void);

/** 把参数写回磁盘（改动后调用；写失败静默，不影响拍摄）。 */
void qzcam_settings_save(void);

/**
 * 把参数里的亮度/对比/镜像/翻转就地应用到一帧 RGB565。
 *
 * 取景线程每帧转换完调用一次；亮度/对比走查表，镜像/翻转是逐行搬运。没有
 * 任何参数需要生效时这个函数几乎不花钱（直接返回）。
 */
void qzcam_settings_apply(uint16_t *pixels, int w, int h);

#endif
