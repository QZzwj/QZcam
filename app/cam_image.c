#define _POSIX_C_SOURCE 200809L
#include "cam_image.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#if defined(QZCAM_HAVE_JPEG)
#include <setjmp.h>
#include <jpeglib.h>
#endif

static int clamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Small range-weighted Gaussian filter: preserve edges before sharpening. */
static void denoise_luma(uint8_t *y, uint8_t *tmp, int w, int h, int strength)
{
    memcpy(tmp, y, (size_t)w * h);
    const int spatial[3] = {1, 2, 1};
    int range = 4 + strength / 6;
    for (int row = 1; row < h - 1; row++) {
        for (int col = 1; col < w - 1; col++) {
            int i = row * w + col, center = tmp[i], sum = 0, weight = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int v = tmp[i + dy * w + dx], delta = abs(v - center);
                    int a = range - delta;
                    if (a <= 0) continue;
                    a *= spatial[dy + 1] * spatial[dx + 1];
                    sum += v * a;
                    weight += a;
                }
            }
            int filtered = weight ? sum / weight : center;
            y[i] = (uint8_t)((center * (100 - strength) + filtered * strength) / 100);
        }
    }
}

static void clahe_luma(uint8_t *y, int w, int h, int strength)
{
    enum { TILES = 8, BINS = 256 };
    uint8_t maps[TILES][TILES][BINS];
    for (int ty = 0; ty < TILES; ty++) {
        for (int tx = 0; tx < TILES; tx++) {
            int hist[BINS] = {0};
            int x0 = tx * w / TILES, x1 = (tx + 1) * w / TILES;
            int y0 = ty * h / TILES, y1 = (ty + 1) * h / TILES;
            int count = (x1 - x0) * (y1 - y0);
            for (int row = y0; row < y1; row++)
                for (int col = x0; col < x1; col++) hist[y[row * w + col]]++;
            int limit = count * 2 / BINS;
            if (limit < 1) limit = 1;
            int excess = 0;
            for (int b = 0; b < BINS; b++) {
                if (hist[b] > limit) { excess += hist[b] - limit; hist[b] = limit; }
            }
            int sum = 0;
            for (int b = 0; b < BINS; b++) {
                hist[b] += excess / BINS + (b < excess % BINS);
                sum += hist[b];
                maps[ty][tx][b] = count ? (uint8_t)(sum * 255 / count) : (uint8_t)b;
            }
        }
    }
    /* Interpolate about tile centers, including clamped edge tiles. */
    for (int row = 0; row < h; row++) {
        int fy = ((row * 2 + 1) * TILES * 128 / h) - 128;
        int ay = fy < 0 ? -1 : fy / 256, by = ay + 1;
        int wy = fy - ay * 256;
        if (ay < 0) ay = 0;
        if (by >= TILES) by = TILES - 1;
        for (int col = 0; col < w; col++) {
            int fx = ((col * 2 + 1) * TILES * 128 / w) - 128;
            int ax = fx < 0 ? -1 : fx / 256, bx = ax + 1;
            int wx = fx - ax * 256;
            if (ax < 0) ax = 0;
            if (bx >= TILES) bx = TILES - 1;
            int i = row * w + col, v = y[i];
            int a = maps[ay][ax][v] * (256 - wx) + maps[ay][bx][v] * wx;
            int b = maps[by][ax][v] * (256 - wx) + maps[by][bx][v] * wx;
            int mapped = (a * (256 - wy) + b * wy + 32768) >> 16;
            y[i] = (uint8_t)((v * (100 - strength) + mapped * strength) / 100);
        }
    }
}

static void sharpen_luma(uint8_t *y, uint8_t *tmp, int w, int h, int strength)
{
    memcpy(tmp, y, (size_t)w * h);
    const int weights[3] = {1, 2, 1};
    for (int row = 1; row < h - 1; row++) {
        for (int col = 1; col < w - 1; col++) {
            int i = row * w + col, blur = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    blur += tmp[i + dy * w + dx] * weights[dy + 1] * weights[dx + 1];
            int detail = (int)tmp[i] - (blur + 8) / 16;
            if (abs(detail) < 3) continue;
            int amount = detail * strength / 100;
            if (amount > 14) amount = 14;
            if (amount < -14) amount = -14;
            y[i] = (uint8_t)clamp(tmp[i] + amount);
        }
    }
}

bool qzcam_image_enhance(uint8_t *nv12, int w, int h, const qzcam_settings_t *s)
{
    if (!nv12 || !s || w < 8 || h < 8 || w > 4096 || h > 4096) return false;
    if (!s->enhancement) return true;
    uint8_t *tmp = malloc((size_t)w * h);
    if (!tmp) return false;
    int denoise = s->denoise;
    int contrast = s->local_contrast;
    int sharp = s->sharpness;
    if (s->enhancement == 2) sharp = sharp * 3 / 2;
    if (s->enhancement == 3) {
        contrast = contrast < 45 ? 45 : contrast;
        denoise = denoise < 60 ? 60 : denoise;
        uint8_t gamma[256];
        for (int i = 0; i < 256; i++) gamma[i] = (uint8_t)(255.0 * pow(i / 255.0, 0.8));
        for (size_t i = 0; i < (size_t)w * h; i++) nv12[i] = gamma[nv12[i]];
    }
    if (denoise) denoise_luma(nv12, tmp, w, h, denoise);
    if (contrast) clahe_luma(nv12, w, h, contrast);
    if (sharp) sharpen_luma(nv12, tmp, w, h, sharp);
    free(tmp);
    return true;
}

uint8_t *qzcam_image_rgb(const uint8_t *nv12, int w, int h, const qzcam_settings_t *s)
{
    if (!nv12 || w <= 0 || h <= 0 || (w & 1) || (h & 1)) return NULL;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int l = nv12[y * w + x] - 16;
            int c = w * h + (y / 2) * w + (x & ~1);
            int u = nv12[c] - 128, v = nv12[c + 1] - 128;
            int ox = s && s->mirror ? w - 1 - x : x;
            int oy = s && s->flip ? h - 1 - y : y;
            uint8_t *p = rgb + ((size_t)oy * w + ox) * 3;
            p[0] = (uint8_t)clamp((298 * l + 409 * v + 128) >> 8);
            p[1] = (uint8_t)clamp((298 * l - 100 * u - 208 * v + 128) >> 8);
            p[2] = (uint8_t)clamp((298 * l + 516 * u + 128) >> 8);
            if (s && (s->brightness || s->contrast != 100)) {
                for (int k = 0; k < 3; k++)
                    p[k] = (uint8_t)clamp(128 + ((p[k] + s->brightness - 128) * s->contrast) / 100);
            }
        }
    }
    return rgb;
}

#if defined(QZCAM_HAVE_JPEG)
typedef struct { struct jpeg_error_mgr base; jmp_buf jump; } jpeg_error_t;
static void jpeg_fail(j_common_ptr c) { longjmp(((jpeg_error_t *)c->err)->jump, 1); }
#endif

bool qzcam_image_jpeg(const char *path, const uint8_t *rgb, int w, int h, int quality)
{
    if (!path || !rgb || w <= 0 || h <= 0) return false;
#if defined(QZCAM_HAVE_JPEG)
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    struct jpeg_compress_struct c;
    jpeg_error_t error;
    memset(&c, 0, sizeof(c));
    c.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_fail;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&c);
        fclose(f);
        unlink(path);
        return false;
    }
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, f);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    c.comp_info[0].h_samp_factor = c.comp_info[0].v_samp_factor = 1;
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)c.next_scanline * w * 3);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
#else
    /* Development hosts can use their installed ffmpeg without SDK libraries. */
    char raw_path[] = "/tmp/qzcam-rgb-XXXXXX";
    int fd = mkstemp(raw_path);
    if (fd < 0) return false;
    size_t left = (size_t)w * h * 3;
    const uint8_t *p = rgb;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) { close(fd); unlink(raw_path); return false; }
        p += n; left -= (size_t)n;
    }
    close(fd);
    char size[40], q[12];
    snprintf(size, sizeof(size), "%dx%d", w, h);
    snprintf(q, sizeof(q), "%d", 2 + (100 - quality) / 5);
    pid_t pid = fork();
    if (pid == 0) {
        execlp("ffmpeg", "ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pixel_format",
               "rgb24", "-video_size", size, "-i", raw_path, "-frames:v", "1", "-q:v", q,
               "-pix_fmt", "yuvj444p", "-f", "image2", path, (char *)NULL);
        _exit(127);
    }
    int status = -1;
    if (pid > 0) while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) break; }
    unlink(raw_path);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

static void le32(uint8_t *p, unsigned v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

#if defined(QZCAM_HAVE_JPEG)
/* 解码 JPEG → out_w × out_h 的 RGB565（等比缩放 + 居中留黑）。
 *
 * 为什么缩放自己做：LVGL 9.2 的软件缩放只能放大，"缩小"（scale < 256）对从文件
 * 流式解码的源一个像素都画不出来（实测整屏黑）；而 libjpeg 自带
 * scale_num/scale_denom 的 1/2、1/4、1/8 缩放解码 —— 既绕开那条坏路，又把解码量
 * 从 3M 像素降到几十 k（1296p 照片开 1/4 只有 576×324，再最近邻缩到面板尺寸）。 */
bool qzcam_image_decode_jpeg_rgb565(const char *path, uint8_t *out, int out_w, int out_h)
{
    if (!path || !out || out_w <= 0 || out_h <= 0) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    struct jpeg_decompress_struct c;
    jpeg_error_t error;
    memset(&c, 0, sizeof(c));
    c.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_fail;

    uint8_t *row = NULL;
    bool ok = false;

    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&c);
    } else {
        jpeg_create_decompress(&c);
        jpeg_stdio_src(&c, f);
        jpeg_read_header(&c, TRUE);

        /* 取最小的 1/N（N ∈ {1,2,4,8}）使解码结果仍不小于目标框 */
        unsigned n = 1;
        while (n < 8 && (c.image_width / (n * 2) >= (JDIMENSION)out_w ||
                         c.image_height / (n * 2) >= (JDIMENSION)out_h))
            n *= 2;
        c.scale_num = 1;
        c.scale_denom = n;

        if (jpeg_start_decompress(&c)) {
            int dw = (int)c.output_width, dh = (int)c.output_height;
            row = malloc((size_t)dw * 3);
            if (row) {
                int fw = out_w, fh = out_h;
                if ((long)dw * out_h > (long)dh * out_w) fh = out_w * dh / dw;  /* 源更宽 */
                else                                     fw = out_h * dw / dh;  /* 源更高 */
                int ox = (out_w - fw) / 2, oy = (out_h - fh) / 2;

                /* 缩小时做**区域平均**而不是单点取样：单点会把丢掉的那些像素
                 * 变成混叠噪点（屏上看着糊且脏）。这里是一次性的事（一张照片一次），
                 * 所以直接上完整方块平均，不做取样折中。 */
                int *acc = malloc(sizeof(int) * (size_t)fw * 3);
                int *cnt = malloc(sizeof(int) * (size_t)fw);
                if (acc && cnt) {
                    memset(out, 0, (size_t)out_w * (size_t)out_h * 2);
                    ok = true;
                    for (int ty = 0; ty < fh && ok; ty++) {
                        int y0 = ty * dh / fh;
                        int y1 = (ty + 1) * dh / fh;
                        if (y1 <= y0) y1 = y0 + 1;
                        if (y1 > dh) y1 = dh;
                        for (int tx = 0; tx < fw; tx++) {
                            int x0 = tx * dw / fw;
                            int x1 = (tx + 1) * dw / fw;
                            if (x1 <= x0) x1 = x0 + 1;
                            if (x1 > dw) x1 = dw;
                            cnt[tx] = (x1 - x0) * (y1 - y0);
                            acc[tx * 3] = acc[tx * 3 + 1] = acc[tx * 3 + 2] = 0;
                        }
                        for (int sy = y0; sy < y1 && ok; sy++) {
                            while ((int)c.output_scanline <= sy) {
                                JSAMPROW rp = row;
                                if (jpeg_read_scanlines(&c, &rp, 1) != 1) break;
                            }
                            if ((int)c.output_scanline <= sy) { ok = false; break; }
                            for (int tx = 0; tx < fw; tx++) {
                                int x0 = tx * dw / fw;
                                int x1 = (tx + 1) * dw / fw;
                                if (x1 <= x0) x1 = x0 + 1;
                                if (x1 > dw) x1 = dw;
                                const uint8_t *p = row + (size_t)x0 * 3;
                                int r = 0, g = 0, b = 0;
                                for (int k = x0; k < x1; k++, p += 3) {
                                    r += p[0];
                                    g += p[1];
                                    b += p[2];
                                }
                                acc[tx * 3] += r;
                                acc[tx * 3 + 1] += g;
                                acc[tx * 3 + 2] += b;
                            }
                        }
                        if (!ok) break;
                        uint16_t *dstrow = (uint16_t *)(out +
                            ((size_t)(oy + ty) * (size_t)out_w + (size_t)ox) * 2);
                        for (int tx = 0; tx < fw; tx++) {
                            int n = cnt[tx] > 0 ? cnt[tx] : 1;
                            unsigned r = (unsigned)(acc[tx * 3] / n);
                            unsigned g = (unsigned)(acc[tx * 3 + 1] / n);
                            unsigned b = (unsigned)(acc[tx * 3 + 2] / n);
                            dstrow[tx] = (uint16_t)(((r & 0xF8) << 8) |
                                                    ((g & 0xFC) << 3) | (b >> 3));
                        }
                    }
                } else {
                    ok = false;
                }
                free(acc);
                free(cnt);
            }
            jpeg_finish_decompress(&c);
        }
        jpeg_destroy_decompress(&c);
    }

    free(row);
    fclose(f);
    return ok;
}
#else
/* 没有 libjpeg 的构建（宿主测试）：不做缩放解码，界面会退回让 LVGL 自己解 */
bool qzcam_image_decode_jpeg_rgb565(const char *path, uint8_t *out, int out_w, int out_h)
{
    (void)path; (void)out; (void)out_w; (void)out_h;
    return false;
}
#endif

bool qzcam_image_bmp(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    unsigned stride = ((unsigned)w * 3 + 3) & ~3u;
    uint8_t head[54] = {0};
    head[0] = 'B'; head[1] = 'M'; head[26] = 1; head[28] = 24;
    le32(head + 2, 54 + stride * h); le32(head + 10, 54); le32(head + 14, 40);
    le32(head + 18, w); le32(head + 22, h); le32(head + 34, stride * h);
    uint8_t *row = calloc(stride, 1);
    bool ok = row && fwrite(head, 1, 54, f) == 54;
    for (int y = h - 1; ok && y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            row[x * 3] = p[2]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[0];
        }
        ok = fwrite(row, 1, stride, f) == stride;
    }
    free(row);
    if (fclose(f) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
}

bool qzcam_image_thumb(const char *path, const uint8_t *rgb, int w, int h)
{
    int tw = 160, th = h * tw / w;
    if (th < 1) th = 1;
    uint8_t *small = malloc((size_t)tw * th * 3);
    if (!small) return false;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++)
            memcpy(small + ((size_t)y * tw + x) * 3,
                   rgb + ((size_t)(y * h / th) * w + x * w / tw) * 3, 3);
    bool ok = qzcam_image_jpeg(path, small, tw, th, 80);
    free(small);
    return ok;
}

bool qzcam_image_preview(const char *path, const uint16_t *rgb565, int w, int h)
{
    uint8_t *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return false;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        unsigned p = rgb565[i];
        rgb[i * 3] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
        rgb[i * 3 + 1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
        rgb[i * 3 + 2] = (uint8_t)((p & 31) * 255 / 31);
    }
    bool ok = qzcam_image_jpeg(path, rgb, w, h, 85);
    free(rgb);
    return ok;
}
