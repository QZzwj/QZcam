#ifndef QZCAM_IMAGE_H
#define QZCAM_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#include "qzcam.h"

/* NV12 input is tightly packed, with an even width and height. */
bool qzcam_image_enhance(uint8_t *nv12, int w, int h, const qzcam_settings_t *s);
uint8_t *qzcam_image_rgb(const uint8_t *nv12, int w, int h, const qzcam_settings_t *s);
bool qzcam_image_jpeg(const char *path, const uint8_t *rgb, int w, int h, int quality);
bool qzcam_image_thumb(const char *path, const uint8_t *rgb, int w, int h);
bool qzcam_image_bmp(const char *path, const uint8_t *rgb, int w, int h);
bool qzcam_image_preview(const char *path, const uint16_t *rgb565, int w, int h);
#endif
