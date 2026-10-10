#include "cam_settings.h"
#include "cam_storage.h"
#include "qzcam.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* 默认参数：时间戳开着（开机即用的取证习惯），其余保守。
 * 模式/录制项按 SC3336 的真实上限给默认：2304x1296 @ 30fps，H.265。 */
static qzcam_settings_t settings = {
    .grid = false,
    .timestamp = true,
    .mirror = false,
    .flip = false,
    .brightness = 0,
    .contrast = 100,
    .fps = 15,
    .selfie_sec = 0,
    .burst = 1,

    .mode = 1,          /* 照片优先：开机即可拍 */
    .resolution = 0,    /* 2304x1296（传感器原生） */
    .rec_fps = 30,
    .codec = 0,         /* H.265 */
    .bitrate_mode = 0,  /* 智能编码 */
    .eis = false,
    .loop_seconds = 180,
    .loop_overwrite = true,

    .hdr = false,
    .wdr = false,
    .isp_denoise = 0,
    .wb_mode = 0,       /* 自动白平衡 */
    .ev = 0,
    .night_mode = false,

    .wb_kelvin = 5500,
    .sharpness = 50,
    .denoise = 50,

    .audio_rec = false,
    .audio_denoise = false,

    .watermark = false,
    .auto_off = 0,      /* 从不 */
    .burst_count = 1,
    .timelapse_ms = 1000,
    .slow_factor = 2,
    .enhancement = 1,
    .local_contrast = 25,
    .jpeg_quality = 95,
    .motion_sensitivity = 50,
    .motion_hold = 10,
    .screen_brightness = 80,
};

qzcam_settings_t *qzcam_settings(void)
{
    return &settings;
}

void qzcam_settings_reset(void)
{
    qzcam_settings_t d = {0};
    d.grid = false;
    d.timestamp = true;
    d.mirror = false;
    d.flip = false;
    d.brightness = 0;
    d.contrast = 100;
    d.fps = 15;
    d.selfie_sec = 0;
    d.burst = 1;

    d.mode = 1;
    d.resolution = 0;
    d.rec_fps = 30;
    d.codec = 0;
    d.bitrate_mode = 0;
    d.eis = false;
    d.loop_seconds = 180;
    d.loop_overwrite = true;

    d.hdr = false;
    d.wdr = false;
    d.isp_denoise = 0;
    d.wb_mode = 0;
    d.ev = 0;
    d.night_mode = false;

    d.wb_kelvin = 5500;
    d.sharpness = 50;
    d.denoise = 50;

    d.audio_rec = false;
    d.audio_denoise = false;

    d.watermark = false;
    d.auto_off = 0;
    d.burst_count = 1;
    d.timelapse_ms = 1000;
    d.slow_factor = 2;
    d.enhancement = 1;
    d.local_contrast = 25;
    d.jpeg_quality = 95;
    d.motion_sensitivity = 50;
    d.motion_hold = 10;
    d.screen_brightness = 80;

    settings = d;
}

/* -------------------------------------------------------- 持久化（kv 文本） */

/* 加新设置项时在表里补一行即可；类型由指针域决定。 */
typedef struct {
    const char *key;
    bool *b;
    int *i;
    int min;
    int max;
} set_entry_t;

static const set_entry_t set_entries[] = {
    { "grid",          &settings.grid,          NULL,               0,   1 },
    { "timestamp",     &settings.timestamp,     NULL,               0,   1 },
    { "mirror",        &settings.mirror,        NULL,               0,   1 },
    { "flip",          &settings.flip,          NULL,               0,   1 },
    { "brightness",    NULL,                    &settings.brightness, -50, 50 },
    { "contrast",      NULL,                    &settings.contrast,  50, 150 },
    { "fps",           NULL,                    &settings.fps,        5,  30 },
    { "mode",          NULL,                    &settings.mode,       0,   4 },
    { "resolution",    NULL,                    &settings.resolution, 0,   2 },
    { "rec_fps",       NULL,                    &settings.rec_fps,   15,  75 },
    { "codec",         NULL,                    &settings.codec,      0,   1 },
    { "bitrate_mode",  NULL,                    &settings.bitrate_mode, 0, 2 },
    { "eis",           &settings.eis,           NULL,               0,   1 },
    { "loop_seconds",  NULL,                    &settings.loop_seconds, 60, 600 },
    { "loop_overwrite",&settings.loop_overwrite,NULL,               0,   1 },
    { "hdr",           &settings.hdr,           NULL,               0,   1 },
    { "wdr",           &settings.wdr,           NULL,               0,   1 },
    { "isp_denoise",   NULL,                    &settings.isp_denoise, 0,  3 },
    { "wb_mode",       NULL,                    &settings.wb_mode,    0,   4 },
    { "ev",            NULL,                    &settings.ev,       -30,  30 },
    { "night_mode",    &settings.night_mode,    NULL,               0,   1 },
    { "wb_kelvin",     NULL,                    &settings.wb_kelvin, 2500, 8000 },
    { "sharpness",     NULL,                    &settings.sharpness,  0, 100 },
    { "denoise",       NULL,                    &settings.denoise,    0, 100 },
    { "audio_rec",     &settings.audio_rec,     NULL,               0,   1 },
    { "audio_denoise", &settings.audio_denoise, NULL,               0,   1 },
    { "watermark",     &settings.watermark,     NULL,               0,   1 },
    { "auto_off",      NULL,                    &settings.auto_off,   0, 300 },
    { "selfie_sec",    NULL,                    &settings.selfie_sec, 0,  10 },
    { "burst",         NULL,                    &settings.burst,      1,  10 },
    { "photo_delay", NULL, &settings.photo_delay, 0, 10 },
    { "burst_count", NULL, &settings.burst_count, 1, 10 },
    { "timelapse_ms", NULL, &settings.timelapse_ms, 500, 10000 },
    { "slow_factor", NULL, &settings.slow_factor, 2, 3 },
    { "enhancement", NULL, &settings.enhancement, 0, 3 },
    { "local_contrast", NULL, &settings.local_contrast, 0, 100 },
    { "jpeg_quality", NULL, &settings.jpeg_quality, 60, 100 },
    { "keep_original", &settings.keep_original, NULL, 0, 1 },
    { "motion_detect", &settings.motion_detect, NULL, 0, 1 },
    { "motion_sensitivity", NULL, &settings.motion_sensitivity, 1, 100 },
    { "motion_hold", NULL, &settings.motion_hold, 3, 60 },
    { "screen_brightness", NULL, &settings.screen_brightness, 5, 100 },
    { "screen_timeout", NULL, &settings.screen_timeout, 0, 300 },
    { "remote_enabled", &settings.remote_enabled, NULL, 0, 1 },
    { "playback_loop", &settings.playback_loop, NULL, 0, 1 },
};

/* 落盘位置：能写哪里放哪里（oem 挂载后最稳，userdata 小，tmp 重启丢） */
static bool settings_path(char *out, size_t size)
{
    const char *override = getenv("QZCAM_SETTINGS");
    if (override && override[0]) {
        snprintf(out, size, "%s", override);
        return true;
    }
    static const char *dirs[] = { "/oem/usr/etc", "/userdata", "/tmp" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char probe[288];
        snprintf(probe, sizeof(probe), "%s/.qzcam-wtest", dirs[i]);
        FILE *fp = fopen(probe, "w");
        if (!fp) continue;
        fclose(fp);
        remove(probe);
        snprintf(out, size, "%s/qzcam-settings.txt", dirs[i]);
        return true;
    }
    return false;
}

void qzcam_settings_save(void)
{
    char path[256];
    if (!settings_path(path, sizeof(path))) return;
    char tmp[260];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;
    for (size_t i = 0; i < sizeof(set_entries) / sizeof(set_entries[0]); i++) {
        const set_entry_t *e = &set_entries[i];
        int value = e->b ? (*e->b ? 1 : 0) : *e->i;
        fprintf(fp, "%s=%d\n", e->key, value);
    }
    fclose(fp);
    rename(tmp, path);   /* 原子替换，断电也不会留半个文件 */

    /* 高速模式（SC3336 640x480@75fps，社区驱动）标记：录像帧率 >30 才需要换
     * 传感器驱动。模块与 IQ 的替换由 S30qzcam 在下次开机时做 —— 运行中换
     * rkaiq 正在用的模块会炸，所以这里只落标记（界面会 toast 提示重启）。 */
    FILE *m = NULL;
    if (settings.rec_fps > 30) {
        m = fopen("/oem/usr/etc/qzcam-hispeed", "w");
        if (m) { fputs("75\n", m); fclose(m); }
    } else {
        remove("/oem/usr/etc/qzcam-hispeed");
    }
    (void)m;
}

void qzcam_settings_load(void)
{
    char path[256];
    if (!settings_path(path, sizeof(path))) return;
    FILE *fp = fopen(path, "r");
    if (!fp) return;   /* 没有历史文件 = 用默认值 */

    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        char key[64] = "";
        int value = 0;
        if (sscanf(line, " %63[^=]=%d", key, &value) != 2) continue;
        /* 去掉 key 尾部的空格 */
        size_t n = strlen(key);
        while (n && (key[n - 1] == ' ' || key[n - 1] == '\t')) key[--n] = '\0';

        for (size_t i = 0; i < sizeof(set_entries) / sizeof(set_entries[0]); i++) {
            const set_entry_t *e = &set_entries[i];
            if (strcmp(key, e->key) != 0) continue;
            if (value < e->min) value = e->min;
            if (value > e->max) value = e->max;
            if (e->b) *e->b = (value != 0);
            else *e->i = value;
            break;
        }
    }
    fclose(fp);
}

/* ------------------------------------------------------------------ 查表 */

/*
 * 亮度/对比是纯逐像素运算，每帧 320x240 = 76800 次 —— 直接算也够快，但既然
 * 值只在拖动时变，就把「5 位通道值 → 修好的 5 位通道值」和 6 位那份都缓存成
 * 表，每帧只剩查表与位运算。
 */
static uint8_t lut5[32];
static uint8_t lut6[64];
static int lut_brightness = 0x7FFFFFFF;
static int lut_contrast = -1;

static inline uint8_t shape(int value, int maxval, int brightness, int contrast)
{
    int v = value * 255 / maxval;                /* 先换到 0..255 域 */
    v += brightness;                             /* 亮度：整体平移 */
    v = 128 + ((v - 128) * contrast) / 100;      /* 对比：围绕中灰缩放 */
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)(v * maxval / 255);
}

static void rebuild_lut(int brightness, int contrast)
{
    if (brightness == lut_brightness && contrast == lut_contrast) return;
    for (int i = 0; i < 32; i++) lut5[i] = shape(i, 31, brightness, contrast);
    for (int i = 0; i < 64; i++) lut6[i] = shape(i, 63, brightness, contrast);
    lut_brightness = brightness;
    lut_contrast = contrast;
}

void qzcam_settings_apply(uint16_t *pixels, int w, int h)
{
    const qzcam_settings_t *s = &settings;
    if (w <= 0 || h <= 0) return;

    bool tone = (s->brightness != 0 || s->contrast != 100);
    if (tone) {
        rebuild_lut(s->brightness, s->contrast);
        size_t count = (size_t)w * (size_t)h;
        for (size_t i = 0; i < count; i++) {
            uint16_t p = pixels[i];
            uint16_t r = (uint16_t)((p >> 11) & 0x1F);
            uint16_t g = (uint16_t)((p >> 5) & 0x3F);
            uint16_t b = (uint16_t)(p & 0x1F);
            pixels[i] = (uint16_t)((lut5[r] << 11) | (lut6[g] << 5) | lut5[b]);
        }
    }

    if (s->mirror) {
        for (int y = 0; y < h; y++) {
            uint16_t *row = pixels + (size_t)y * w;
            for (int x = 0, x2 = w - 1; x < x2; x++, x2--) {
                uint16_t tmp = row[x];
                row[x] = row[x2];
                row[x2] = tmp;
            }
        }
    }

    if (s->flip) {
        for (int y = 0, y2 = h - 1; y < y2; y++, y2--) {
            uint16_t *a = pixels + (size_t)y * w;
            uint16_t *b = pixels + (size_t)y2 * w;
            for (int x = 0; x < w; x++) {
                uint16_t tmp = a[x];
                a[x] = b[x];
                b[x] = tmp;
            }
        }
    }
}

/* -------------------------------------------------------------- 存图目录 */

/* 落地目录统一由 cam_storage 决定（SD 卡优先），这里只是转发，避免两处逻辑 */
const char *qzcam_settings_photo_dir(void)
{
    return qzcam_storage_dir();
}
