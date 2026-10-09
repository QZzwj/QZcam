#include "cam_record.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cam_storage.h"
#include "qzcam.h"

/*
 * AVI 结构（单视频流，16 位 RGB565 未压缩）：
 *
 *   'RIFF' <size> 'AVI '
 *     LIST 'hdrl'
 *       'avih' 56  <MainAVIHeader>            ← 帧数在这里回填
 *       LIST 'strl'
 *         'strh' 56  <AVIStreamHeader>        ← 帧数在这里回填
 *         'strf' 52  <BITMAPINFOHEADER + 3 个位域掩码>
 *     LIST 'movi'
 *       '00dc' <size> <像素> [补 0 对齐]      ← 每帧一个
 *     'idx1' <size> <索引项…>                 ← 停止时写
 *
 * 像素是 DIB 约定：**行自下而上**（和 BMP 一样），16 位 565，掩码走 BI_BITFIELDS，
 * 这样 VLC/ffmpeg 认得出颜色（否则会被当成 555，偏色）。
 */

#define BI_BITFIELDS 3u
#define AVIF_HASINDEX 0x10u
#define AVIIF_KEYFRAME 0x10u
#define MAX_FRAMES 20000            /* 索引项上限：10fps 下约 33 分钟，防内存无限涨 */

typedef struct {
    uint32_t offset;   /* '00dc' fourcc 相对 movi 数据区的偏移 */
    uint32_t size;
} index_entry_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static FILE *fp;
static bool active;
static int width, height;
static int fps = 15;
static uint32_t frame_count;
static unsigned long long bytes_written;
static unsigned long long started_ms;
static unsigned long long last_frame_ms;
static char file_path[240];

static index_entry_t *index_entries;
static uint32_t index_count;
static long riff_size_pos, movi_size_pos, movi_data_start;
static long avih_frames_pos, strh_length_pos;

/* ------------------------------------------------------------------ 基础写 */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static long tell(FILE *f) { return ftell(f); }

static void write_fourcc(const char *tag) { fwrite(tag, 1, 4, fp); }
static void write_u32(uint32_t v)
{
    uint8_t b[4];
    put_u32(b, v);
    fwrite(b, 1, 4, fp);
}
static void write_u16(uint16_t v)
{
    uint8_t b[2];
    put_u16(b, v);
    fwrite(b, 1, 2, fp);
}

static unsigned long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ 头部 */

static void write_header(void)
{
    uint32_t micro_per_frame = (uint32_t)(1000000u / (uint32_t)(fps > 0 ? fps : 15));
    uint32_t image_size = (uint32_t)width * (uint32_t)height * 2u;

    write_fourcc("RIFF");
    riff_size_pos = tell(fp);
    write_u32(0);                       /* 回填 */
    write_fourcc("AVI ");

    write_fourcc("LIST");
    write_u32(4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 52);   /* hdrl 总长（下面各块之和） */
    write_fourcc("hdrl");

    /* avih：MainAVIHeader */
    write_fourcc("avih");
    write_u32(56);
    write_u32(micro_per_frame);
    write_u32(image_size * (uint32_t)(fps > 0 ? fps : 15));   /* dwMaxBytesPerSec */
    write_u32(0);                       /* dwPaddingGranularity */
    write_u32(AVIF_HASINDEX);
    avih_frames_pos = tell(fp);
    write_u32(0);                       /* dwTotalFrames，回填 */
    write_u32(0);                       /* dwInitialFrames */
    write_u32(1);                       /* dwStreams */
    write_u32(image_size);              /* dwSuggestedBufferSize */
    write_u32((uint32_t)width);
    write_u32((uint32_t)height);
    write_u32(0); write_u32(0); write_u32(0); write_u32(0);   /* dwReserved[4] */

    write_fourcc("LIST");
    write_u32(4 + 8 + 56 + 8 + 52);     /* strl */
    write_fourcc("strl");

    /* strh：AVIStreamHeader */
    write_fourcc("strh");
    write_u32(56);
    write_fourcc("vids");
    write_fourcc("DIB ");               /* 未压缩位图 */
    write_u32(0);                       /* dwFlags */
    write_u16(0);                       /* wPriority */
    write_u16(0);                       /* wLanguage */
    write_u32(0);                       /* dwInitialFrames */
    write_u32(1000);                    /* dwScale */
    write_u32((uint32_t)(fps > 0 ? fps : 15) * 1000u);   /* dwRate → rate/scale = fps */
    write_u32(0);                       /* dwStart */
    strh_length_pos = tell(fp);
    write_u32(0);                       /* dwLength（帧数），回填 */
    write_u32(image_size);              /* dwSuggestedBufferSize */
    write_u32(0xFFFFFFFFu);             /* dwQuality = -1 */
    write_u32(0);                       /* dwSampleSize */
    write_u16(0); write_u16(0); write_u16((uint16_t)width); write_u16((uint16_t)height);  /* rcFrame */

    /* strf：BITMAPINFOHEADER + 位域掩码 */
    write_fourcc("strf");
    write_u32(52);
    write_u32(40);                      /* biSize */
    write_u32((uint32_t)width);
    write_u32((uint32_t)height);
    write_u16(1);                       /* biPlanes */
    write_u16(16);                      /* biBitCount */
    write_u32(BI_BITFIELDS);
    write_u32(image_size);              /* biSizeImage */
    write_u32(0); write_u32(0);         /* 分辨率 */
    write_u32(0); write_u32(0);         /* 调色板 */
    write_u32(0x0000F800u);             /* R 掩码 */
    write_u32(0x000007E0u);             /* G 掩码 */
    write_u32(0x0000001Fu);             /* B 掩码 */

    write_fourcc("LIST");
    movi_size_pos = tell(fp);
    write_u32(0);                       /* 回填 */
    write_fourcc("movi");
    movi_data_start = tell(fp);
}

/* -------------------------------------------------------------- 单帧写入 */

static void write_frame(const uint8_t *rgb565, int w, int h)
{
    uint32_t size = (uint32_t)w * (uint32_t)h * 2u;
    long chunk_start = tell(fp);

    write_fourcc("00dc");
    write_u32(size);

    /* DIB 是自下而上：整行倒着写 */
    const uint8_t *row = rgb565 + (size_t)(h - 1) * (size_t)w * 2u;
    for (int y = 0; y < h; y++) {
        fwrite(row, 1, (size_t)w * 2u, fp);
        row -= (size_t)w * 2u;
    }
    if (size & 1u) fputc(0, fp);        /* 4 字节对齐的填充（这里尺寸总是偶数，保险起见） */

    if (index_count < MAX_FRAMES) {
        index_entries[index_count].offset = (uint32_t)(chunk_start - movi_data_start);
        index_entries[index_count].size = size;
        index_count++;
    }
    frame_count++;
    bytes_written += size + 8;
}

/* ------------------------------------------------------------------ 对外 */

bool qzcam_rec_start(char *path, unsigned path_size)
{
    pthread_mutex_lock(&lock);
    if (active) {
        pthread_mutex_unlock(&lock);
        return false;
    }

    const qzcam_settings_t *s = qzcam_settings();
    width = qzcam_panel_w;
    height = qzcam_panel_h;
    fps = s->fps > 0 ? s->fps : 15;
    if (fps > 30) fps = 30;
    if (width <= 0 || height <= 0) {
        pthread_mutex_unlock(&lock);
        return false;
    }

    time_t now = time(NULL);
    struct tm parts;
    localtime_r(&now, &parts);
    snprintf(file_path, sizeof(file_path), "%s/VID_%04d%02d%02d_%02d%02d%02d.avi",
             qzcam_storage_dir(), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
             parts.tm_hour, parts.tm_min, parts.tm_sec);

    fp = fopen(file_path, "wb");
    if (!fp) {
        pthread_mutex_unlock(&lock);
        return false;
    }

    if (!index_entries) index_entries = malloc(sizeof(index_entry_t) * MAX_FRAMES);
    index_count = 0;
    frame_count = 0;
    bytes_written = 0;
    started_ms = monotonic_ms();
    last_frame_ms = 0;

    write_header();
    active = true;
    pthread_mutex_unlock(&lock);

    if (path && path_size) snprintf(path, path_size, "%s", file_path);
    return true;
}

void qzcam_rec_stop(void)
{
    pthread_mutex_lock(&lock);
    if (!active) {
        pthread_mutex_unlock(&lock);
        return;
    }
    active = false;

    /* idx1 索引 */
    long idx_start = tell(fp);
    write_fourcc("idx1");
    write_u32(index_count * 16u);
    for (uint32_t i = 0; i < index_count; i++) {
        write_fourcc("00dc");
        write_u32(AVIIF_KEYFRAME);
        write_u32(index_entries[i].offset);
        write_u32(index_entries[i].size);
    }
    long end = tell(fp);

    /* 回填各处长度 */
    fseek(fp, riff_size_pos, SEEK_SET);
    write_u32((uint32_t)(end - 8));
    fseek(fp, movi_size_pos, SEEK_SET);
    write_u32((uint32_t)(idx_start - movi_size_pos - 4));
    fseek(fp, avih_frames_pos, SEEK_SET);
    write_u32(frame_count);
    fseek(fp, strh_length_pos, SEEK_SET);
    write_u32(frame_count);
    fseek(fp, end, SEEK_SET);

    fclose(fp);
    fp = NULL;
    pthread_mutex_unlock(&lock);
}

bool qzcam_rec_active(void)
{
    pthread_mutex_lock(&lock);
    bool value = active;
    pthread_mutex_unlock(&lock);
    return value;
}

int qzcam_rec_seconds(void)
{
    pthread_mutex_lock(&lock);
    int seconds = active ? (int)((monotonic_ms() - started_ms) / 1000ULL) : 0;
    pthread_mutex_unlock(&lock);
    return seconds;
}

int qzcam_rec_frames(void)
{
    pthread_mutex_lock(&lock);
    int frames = (int)frame_count;
    pthread_mutex_unlock(&lock);
    return frames;
}

unsigned long long qzcam_rec_bytes(void)
{
    pthread_mutex_lock(&lock);
    unsigned long long bytes = bytes_written;
    pthread_mutex_unlock(&lock);
    return bytes;
}

void qzcam_rec_submit(const uint8_t *rgb565, int w, int h)
{
    if (!rgb565) return;

    pthread_mutex_lock(&lock);
    if (!active || !fp) {
        pthread_mutex_unlock(&lock);
        return;
    }
    /* 按目标帧率抽帧：取景可能跑在 15~30fps，录像帧率由 settings->fps 决定 */
    unsigned long long now = monotonic_ms();
    if (last_frame_ms != 0 && now - last_frame_ms < (unsigned long long)(1000 / (fps > 0 ? fps : 15))) {
        pthread_mutex_unlock(&lock);
        return;
    }
    last_frame_ms = now;

    if (w == width && h == height) write_frame(rgb565, w, h);
    pthread_mutex_unlock(&lock);
}
