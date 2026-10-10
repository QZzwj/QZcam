#ifndef QZCAM_IMAGE_H
#define QZCAM_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#include "qzcam.h"

/* NV12 input is tightly packed, with an even width and height. */
bool qzcam_image_enhance(uint8_t *nv12, int w, int h, const qzcam_settings_t *s);
uint8_t *qzcam_image_rgb(const uint8_t *nv12, int w, int h, const qzcam_settings_t *s);
bool qzcam_image_jpeg(const char *path, const uint8_t *rgb, int w, int h, int quality);

/**
 * 把一张 JPEG 解码成 out_w × out_h 的 RGB565（等比缩放，剩余留黑），给界面当
 * draw buffer 直接画。用 libjpeg 的 1/2、1/4、1/8 缩放解码，比让 LVGL 从
 * 1296p 原图采样既干净又快。只吃 JPEG；BMP 或无 libjpeg 的构建返回 false。
 */
bool qzcam_image_decode_jpeg_rgb565(const char *path, uint8_t *out, int out_w, int out_h);
bool qzcam_image_thumb(const char *path, const uint8_t *rgb, int w, int h);
bool qzcam_image_bmp(const char *path, const uint8_t *rgb, int w, int h);
bool qzcam_image_preview(const char *path, const uint16_t *rgb565, int w, int h);
#endif
