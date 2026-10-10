#include "cam_record.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cam_encoder.h"
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
static char stop_reason[96];      /* 非人为停止的原因（写失败/空间不足），UI 弹提示 */

static int open_segment_locked(void);
static void finalize_locked(void);
static int rec_kind;              /* 0=普通录像（submit 抽帧节流） 1=延时摄影（submit_now 直写） */

/* 落盘后端：硬件编码走 MP4（VENC + rkmuxer），没有编码器时退回未压缩 AVI。
 * MP4 的编码器是「第一帧到了才知道 VI 的真实尺寸」——所以开录时先只定文件名，
 * 编码通道留到 submit_isp 的第一帧再建（enc_pending）。 */
enum { BACKEND_AVI = 0, BACKEND_MP4 = 1 };
static int backend = BACKEND_AVI;
static bool enc_pending;
static int enc_w, enc_h;

static bool start_encoder_locked(void);
static void stop_backend_locked(void);

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

static int write_frame(const uint8_t *rgb565, int w, int h)
{
    uint32_t size = (uint32_t)w * (uint32_t)h * 2u;
    long chunk_start = tell(fp);

    write_fourcc("00dc");
    write_u32(size);

    /* DIB 是自下而上：整行倒着写。任何一次写失败都返回 0（= 存储出问题了） */
    const uint8_t *row = rgb565 + (size_t)(h - 1) * (size_t)w * 2u;
    for (int y = 0; y < h; y++) {
        if (fwrite(row, 1, (size_t)w * 2u, fp) != (size_t)w * 2u) return 0;
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
    return 1;
}

/* ------------------------------------------------------------------ 对外 */

/* 时间戳文件名（AVI/MP4 共用一套命名，字典序 = 时间序，循环覆盖按它挑最旧的） */
static void make_record_path(char *out, size_t size, const char *ext)
{
    time_t now = time(NULL);
    struct tm parts;
    localtime_r(&now, &parts);
    snprintf(out, size, "%s/VID_%04d%02d%02d_%02d%02d%02d.%s",
             qzcam_storage_dir(), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
             parts.tm_hour, parts.tm_min, parts.tm_sec, ext);
}

/* 码率上限（kbps）。基准按像素量给（约 0.07 bit/像素/帧 @30fps：1296p≈6.2Mbps、
 * 1080p≈4.3Mbps、720p≈1.9Mbps），再乘上「录制画质」档位。
 *
 * 档位是按真机实测口径定的（1296p + 智能编码/AVBR，静止为主的画面）：
 *   高画质 = 上限本身 → 实测平均 4.1Mbps ≈ 31MB/分钟
 *   标准   = 上限 1/4 → ≈ 8MB/分钟
 *   省空间 = 上限 1/16 → ≈ 2MB/分钟
 * AVBR 只把上限当天花板，静止画面会自动用得更少，所以实测平均值低于上限。 */
static int encode_bitrate_kbps(int w, int h)
{
    long base = (long)w * (long)h / 480;
    if (base < 256) base = 256;
    if (base > 20000) base = 20000;

    long kbps = base;
    switch (qzcam_settings()->rec_quality) {
    case 0:  kbps = base / 16; break;   /* 省空间 */
    case 1:  kbps = base / 4;  break;   /* 标准 */
    default: kbps = base;      break;   /* 高画质 */
    }
    if (kbps < 128) kbps = 128;
    return (int)kbps;
}

int qzcam_rec_bitrate_kbps(void)
{
    /* 与 cam_core 的 ISP 出图尺寸保持一致（分辨率设置 → 画面尺寸） */
    static const int sizes[3][2] = { {2304, 1296}, {1920, 1080}, {1280, 720} };
    int res = qzcam_settings()->resolution;
    if (res < 0 || res > 2) res = 0;
    return encode_bitrate_kbps(sizes[res][0], sizes[res][1]);
}

/* 起硬件编码这条路：先只定文件名。编码通道要等第一帧到了、知道 VI 的真实
 * 尺寸（含 stride）再建 —— 开录那一刻还没有帧可以参考。 */
static bool start_encoder_locked(void)
{
    if (!qzcam_enc_available()) return false;
    make_record_path(file_path, sizeof(file_path), "mp4");
    enc_pending = true;
    enc_w = enc_h = 0;
    return true;
}

/* 收掉当前段（持锁调用） */
static void stop_backend_locked(void)
{
    if (backend == BACKEND_MP4) {
        if (!enc_pending) qzcam_enc_close();
        enc_pending = false;
    } else {
        finalize_locked();
    }
}

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
    fps = s->rec_fps > 0 ? s->rec_fps : (s->fps > 0 ? s->fps : 15);
    if (fps > 75) fps = 75;   /* 高速模式传感器实际出 75fps，录像最多跟到这 */

    stop_reason[0] = '\0';
    rec_kind = 0;                        /* 普通录像：submit 走抽帧节流 */
    frame_count = 0;
    bytes_written = 0;
    index_count = 0;
    started_ms = monotonic_ms();
    last_frame_ms = 0;

    /* 有硬件编码器就录 H.264/H.265 的 MP4；没有（模拟器，或 ISP 没起流）退回
     * 未压缩 RGB565 的 AVI —— 那条路不依赖媒体库，永远能录。 */
    if (start_encoder_locked()) {
        backend = BACKEND_MP4;
        active = true;
        pthread_mutex_unlock(&lock);
        if (path && path_size) snprintf(path, path_size, "%s", file_path);
        return true;
    }

    if (width <= 0 || height <= 0) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    backend = BACKEND_AVI;
    if (!open_segment_locked()) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    active = true;
    pthread_mutex_unlock(&lock);

    if (path && path_size) snprintf(path, path_size, "%s", file_path);
    return true;
}

/* 收尾当前段：写 idx1 + 回填长度 + 关文件（持锁调用） */
static void finalize_locked(void)
{
    if (!fp) return;

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

    fseek(fp, riff_size_pos, SEEK_SET);
    write_u32((uint32_t)(end - 8));
    fseek(fp, movi_size_pos, SEEK_SET);
    write_u32((uint32_t)(idx_start - movi_size_pos - 4));
    fseek(fp, avih_frames_pos, SEEK_SET);
    write_u32(frame_count);
    fseek(fp, strh_length_pos, SEEK_SET);
    write_u32(frame_count);
    fflush(fp);
    fseek(fp, end, SEEK_SET);

    fclose(fp);
    fp = NULL;
}

/* 开新段：生成时间戳文件名 + 写头 + 清零计数（持锁调用）。1 = 成功 */
static int open_segment_locked(void)
{
    make_record_path(file_path, sizeof(file_path), "avi");

    fp = fopen(file_path, "wb");
    if (!fp) return 0;

    if (!index_entries) index_entries = malloc(sizeof(index_entry_t) * MAX_FRAMES);
    index_count = 0;
    frame_count = 0;
    bytes_written = 0;
    started_ms = monotonic_ms();
    last_frame_ms = 0;

    write_header();
    return 1;
}

void qzcam_rec_stop(void)
{
    pthread_mutex_lock(&lock);
    if (!active) {
        pthread_mutex_unlock(&lock);
        return;
    }
    active = false;
    stop_backend_locked();
    pthread_mutex_unlock(&lock);
}

/* 循环覆盖：删掉最旧的一段录像（文件名时间戳字典序 = 时间序），
 * 跳过当前正在写的文件。 */
static void delete_oldest_video(const char *keep_path)
{
    DIR *dir = opendir(qzcam_storage_dir());
    if (!dir) return;
    char oldest_name[256] = "";
    char oldest_full[512] = "";
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (!strstr(item->d_name, ".avi") && !strstr(item->d_name, ".mp4")) continue;
        if (!oldest_name[0] || strcmp(item->d_name, oldest_name) < 0) {
            snprintf(oldest_name, sizeof(oldest_name), "%s", item->d_name);
            snprintf(oldest_full, sizeof(oldest_full), "%s/%s",
                     qzcam_storage_dir(), item->d_name);
        }
    }
    closedir(dir);
    if (oldest_name[0] && strcmp(oldest_full, keep_path) != 0)
        unlink(oldest_full);
}

/* 编码器可能自己停了（写盘失败）：收尾并把原因记进 stop_reason。
 * 持锁调用；返回 true = 还在录。 */
static bool encoder_alive_locked(void)
{
    if (backend != BACKEND_MP4 || enc_pending) return true;
    if (qzcam_enc_active()) return true;

    qzcam_enc_close();                     /* 落 moov，别留下半个文件 */
    active = false;
    const char *why = qzcam_enc_last_error();
    snprintf(stop_reason, sizeof(stop_reason), "%s", why[0] ? why : "编码录像已停止");
    return false;
}

bool qzcam_rec_active(void)
{
    pthread_mutex_lock(&lock);
    bool value = active && encoder_alive_locked();
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

const char *qzcam_rec_stop_reason(void)
{
    return stop_reason[0] ? stop_reason : "";
}

bool qzcam_rec_tl_start(char *path, unsigned path_size, int out_fps)
{
    pthread_mutex_lock(&lock);
    if (active) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    width = qzcam_panel_w;
    height = qzcam_panel_h;
    fps = out_fps > 0 ? out_fps : 15;    /* 延时视频的播放帧率 */
    if (fps > 30) fps = 30;
    rec_kind = 1;
    stop_reason[0] = '\0';
    backend = BACKEND_AVI;      /* 延时按间隔送帧，没有连续 NV12，走未压缩 AVI */
    enc_pending = false;
    if (!open_segment_locked()) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    active = true;
    pthread_mutex_unlock(&lock);
    if (path && path_size) snprintf(path, path_size, "%s", file_path);
    return true;
}

void qzcam_rec_submit_now(const uint8_t *rgb565, int w, int h)
{
    if (!rgb565) return;
    pthread_mutex_lock(&lock);
    if (!active || !fp || rec_kind != 1) {
        pthread_mutex_unlock(&lock);
        return;
    }
    if (w == width && h == height) {
        if (!write_frame(rgb565, w, h)) {
            finalize_locked();
            active = false;
            snprintf(stop_reason, sizeof(stop_reason), "写入失败（卡满/卡拔出？）延时已保存");
        }
    }
    pthread_mutex_unlock(&lock);
}

int qzcam_rec_frames(void)
{
    pthread_mutex_lock(&lock);
    int frames = (backend == BACKEND_MP4 && !enc_pending)
                     ? qzcam_enc_frames() : (int)frame_count;
    pthread_mutex_unlock(&lock);
    return frames;
}

unsigned long long qzcam_rec_bytes(void)
{
    pthread_mutex_lock(&lock);
    unsigned long long bytes = (backend == BACKEND_MP4 && !enc_pending)
                                   ? qzcam_enc_bytes() : bytes_written;
    pthread_mutex_unlock(&lock);
    return bytes;
}

/* 目标帧率的抽帧节流（取景可能跑在 30fps，录像帧率由 settings 决定）。
 * 持锁调用；返回 false = 这一帧不要。 */
static bool frame_due_locked(unsigned long long now)
{
    if (last_frame_ms != 0 && now - last_frame_ms < (unsigned long long)(1000 / (fps > 0 ? fps : 15)))
        return false;
    last_frame_ms = now;
    return true;
}

/* 循环录像：到分段时长就换新文件；空间紧张时按设置覆盖最旧段或停录。
 * 持锁调用；返回 false = 已经停录了。 */
static bool rotate_if_needed_locked(unsigned long long now)
{
    const qzcam_settings_t *st = qzcam_settings();
    unsigned loop_sec = (unsigned)(st->loop_seconds > 0 ? st->loop_seconds : 0);
    if (!loop_sec || (now - started_ms) / 1000ULL < loop_sec) return true;

    stop_backend_locked();
    if (qzcam_storage_free_bytes() < (150ULL << 20)) {
        if (st->loop_overwrite) {
            delete_oldest_video(file_path);
        } else {
            active = false;
            snprintf(stop_reason, sizeof(stop_reason), "存储空间不足（<150MB），录像已停止");
            return false;
        }
    }

    if (backend == BACKEND_MP4) {
        if (!start_encoder_locked()) {
            active = false;
            snprintf(stop_reason, sizeof(stop_reason), "新分段创建失败，录像已停止");
            return false;
        }
    } else if (!open_segment_locked()) {
        active = false;
        snprintf(stop_reason, sizeof(stop_reason), "新分段创建失败，录像已停止");
        return false;
    }
    return true;
}

void qzcam_rec_submit_isp(void *video_frame, int w, int h, int stride, int vir_h)
{
    if (!video_frame || w <= 0 || h <= 0) return;

    pthread_mutex_lock(&lock);
    if (!active || backend != BACKEND_MP4) {
        pthread_mutex_unlock(&lock);
        return;
    }

    /* 第一帧到了才知道 VI 的真实尺寸，编码通道这时候才建得出来 */
    if (enc_pending) {
        const qzcam_settings_t *s = qzcam_settings();
        if (!qzcam_enc_open(file_path, w, h, stride, vir_h, fps, s->codec,
                            s->bitrate_mode, encode_bitrate_kbps(w, h))) {
            /* 编码起不来就别硬撑：退回未压缩 AVI，至少画面留得下来 */
            fprintf(stderr, "QZcam: 硬件编码不可用（%s），退回未压缩 AVI\n",
                    qzcam_enc_last_error());
            backend = BACKEND_AVI;
            if (!open_segment_locked()) {
                active = false;
                snprintf(stop_reason, sizeof(stop_reason), "录像启动失败（存储不可写？）");
            }
            pthread_mutex_unlock(&lock);
            return;
        }
        enc_pending = false;
        enc_w = w;
        enc_h = h;
        started_ms = monotonic_ms();     /* 从真正出画那一刻开始计时 */
        last_frame_ms = 0;
    }

    /* 取景重连后尺寸变了：当前段的码流尺寸已经定死，换新段更稳妥 */
    if (w != enc_w || h != enc_h) {
        stop_backend_locked();
        if (!start_encoder_locked()) {
            active = false;
            snprintf(stop_reason, sizeof(stop_reason), "分辨率变化，录像已停止");
        }
        pthread_mutex_unlock(&lock);
        return;
    }

    unsigned long long now = monotonic_ms();
    if (!frame_due_locked(now)) {
        pthread_mutex_unlock(&lock);
        return;
    }

    if (!qzcam_enc_send(video_frame)) {
        encoder_alive_locked();           /* 出错的话它已经自己停了，这里把状态同步掉 */
        pthread_mutex_unlock(&lock);
        return;
    }
    rotate_if_needed_locked(now);
    pthread_mutex_unlock(&lock);
}

void qzcam_rec_submit(const uint8_t *rgb565, int w, int h)
{
    if (!rgb565) return;

    pthread_mutex_lock(&lock);
    if (!active || !fp || backend != BACKEND_AVI) {
        /* 走硬件编码时帧从 submit_isp() 进来，这条（RGB565）不用管 */
        pthread_mutex_unlock(&lock);
        return;
    }
    /* 按目标帧率抽帧：取景可能跑在 15~30fps，录像帧率由 settings->fps 决定 */
    unsigned long long now = monotonic_ms();
    if (!frame_due_locked(now)) {
        pthread_mutex_unlock(&lock);
        return;
    }

    if (w == width && h == height) {
        if (!write_frame(rgb565, w, h)) {
            /* 写失败 = 存储出问题：立刻收尾本段并停，别让坏文件继续长大 */
            finalize_locked();
            active = false;
            snprintf(stop_reason, sizeof(stop_reason), "写入失败（卡满/卡拔出？）录像已保存");
            pthread_mutex_unlock(&lock);
            return;
        }
        rotate_if_needed_locked(now);
    }
    pthread_mutex_unlock(&lock);
}

/* =====================================================================
 * 异常录像恢复：掉电后的孤儿 AVI 索引重建
 *
 * 正常收尾（finalize_locked）会写 idx1、回填 RIFF/movi 长度、avih/strh 帧数。
 * 中途掉电/拔卡只剩一个「写了一半」的文件：头是完整的（开录时就写了），但上面
 * 那几样全是 0 —— 播放器要么不认，要么按 0 帧处理。
 *
 * 这里不依赖索引，直接从 movi 里逐个 `00dc` 块重新数帧，把索引与长度补回去。
 * 只认自家写出来的那种 AVI（RIFF/AVI + LIST movi），别的文件一律不动。
 * ===================================================================== */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 在 [start,end) 的块序列里找 fourcc，返回其数据区绝对偏移（没找到 -1）。
 * 只递归进 LIST 容器，不会误伤 payload 里凑巧出现的四个字节 —— 这正是
 * avih/strh 嵌在 LIST hdrl / LIST strl 里必须的处理方式。 */
static long find_chunk(FILE *f, long start, long end, const char *fourcc,
                       uint32_t *size_out, int depth)
{
    if (depth > 4) return -1;
    long p = start;
    while (p + 8 <= end) {
        uint8_t hdr[8];
        if (fseek(f, p, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) break;
        uint32_t size = rd_u32(hdr + 4);
        if (!memcmp(hdr, "LIST", 4)) {
            uint8_t kind[4];
            if (fread(kind, 1, 4, f) != 4) break;
            long inner = p + 12;
            long inner_end = p + 8 + (long)size;
            if (inner_end > end) inner_end = end;
            long found = find_chunk(f, inner, inner_end, fourcc, size_out, depth + 1);
            if (found >= 0) return found;
        } else if (!memcmp(hdr, fourcc, 4)) {
            if (size_out) *size_out = size;
            return p + 8;
        }
        p += 8 + (long)((size + 1) & ~1u);
    }
    return -1;
}

/* 修一个文件；真的动了才返回 true。 */
static bool repair_one(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long fsize = ftell(f);
    uint8_t head[12];
    if (fsize < 64 || fseek(f, 0, SEEK_SET) != 0 ||
        fread(head, 1, 12, f) != 12 ||
        memcmp(head, "RIFF", 4) || memcmp(head + 8, "AVI ", 4)) {
        fclose(f);
        return false;
    }

    /* 已经收尾过：RIFF 长度与文件实际大小对得上。这一步是 O(1)，
     * 正常文件（绝大多数）到此为止，不会为了修一个坏文件去扫整个目录。 */
    uint32_t riff_size = rd_u32(head + 4);
    if (riff_size != 0 && (long)riff_size + 8 == fsize) {
        fclose(f);
        return false;
    }

    /* 找 LIST movi：只走顶层，遇到 movi 就停（孤儿文件的 movi 长度字段是 0，
     * 不能按它跳，否则会跳进帧数据里乱认）。 */
    long movi_pos = -1;
    long p = 12;
    while (p + 8 <= fsize) {
        uint8_t hdr[8];
        if (fseek(f, p, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) break;
        uint32_t size = rd_u32(hdr + 4);
        if (!memcmp(hdr, "LIST", 4)) {
            uint8_t kind[4];
            if (fread(kind, 1, 4, f) != 4) break;
            if (!memcmp(kind, "movi", 4)) { movi_pos = p; break; }
            p += 12;
        } else {
            p += 8 + (long)((size + 1) & ~1u);
        }
    }
    if (movi_pos < 0) {          /* 不是自家 AVI（比如新录的 mp4），别碰 */
        fclose(f);
        return false;
    }

    /* 数帧：一直读到文件尾。掉电时最后一帧可能只写了一半，按「放不下整块」
     * 判定为残缺，丢掉它。 */
    long movi_data = movi_pos + 12;
    size_t cap = 1024, count = 0;
    index_entry_t *ents = malloc(sizeof(index_entry_t) * cap);
    if (!ents) { fclose(f); return false; }

    long scan = movi_data;
    while (scan + 8 <= fsize) {
        uint8_t hdr[8];
        if (fseek(f, scan, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) break;
        uint32_t size = rd_u32(hdr + 4);
        bool video = !memcmp(hdr, "00dc", 4) || !memcmp(hdr, "00db", 4);
        if (!video) break;                                  /* 撞到 idx1/重叠区就停 */
        if (scan + 8 + (long)size > fsize) break;           /* 尾帧残缺 */
        if (count == cap) {
            cap *= 2;
            index_entry_t *bigger = realloc(ents, sizeof(index_entry_t) * cap);
            if (!bigger) break;
            ents = bigger;
        }
        ents[count].offset = (uint32_t)(scan - movi_data);
        ents[count].size = size;
        count++;
        scan += 8 + (long)((size + 1) & ~1u);
    }
    if (count == 0) { free(ents); fclose(f); return false; }

    /* 截掉尾部的残帧字节，再从末尾补 idx1 */
    if (ftruncate(fileno(f), scan) != 0) { free(ents); fclose(f); return false; }
    long idx_start = scan;
    uint8_t b[4];
    fseek(f, idx_start, SEEK_SET);
    fwrite("idx1", 1, 4, f);
    put_u32(b, (uint32_t)count * 16u);
    fwrite(b, 1, 4, f);
    for (size_t i = 0; i < count; i++) {
        fwrite("00dc", 1, 4, f);
        put_u32(b, AVIIF_KEYFRAME);  fwrite(b, 1, 4, f);
        put_u32(b, ents[i].offset);  fwrite(b, 1, 4, f);
        put_u32(b, ents[i].size);    fwrite(b, 1, 4, f);
    }
    long end = idx_start + 8 + (long)count * 16;

    /* 回填三个长度 + 两处帧数（avih/strh 的字段偏移见 AVI 规范） */
    fseek(f, 4, SEEK_SET);
    put_u32(b, (uint32_t)(end - 8)); fwrite(b, 1, 4, f);
    fseek(f, movi_pos + 4, SEEK_SET);
    put_u32(b, (uint32_t)(idx_start - (movi_pos + 4) - 4)); fwrite(b, 1, 4, f);

    uint32_t chunk_size = 0;
    long avih = find_chunk(f, 12, movi_pos, "avih", &chunk_size, 0);
    if (avih >= 0 && chunk_size >= 20) {
        fseek(f, avih + 16, SEEK_SET);      /* dwTotalFrames */
        put_u32(b, (uint32_t)count); fwrite(b, 1, 4, f);
    }
    long strh = find_chunk(f, 12, movi_pos, "strh", &chunk_size, 0);
    if (strh >= 0 && chunk_size >= 36) {
        fseek(f, strh + 32, SEEK_SET);      /* dwLength */
        put_u32(b, (uint32_t)count); fwrite(b, 1, 4, f);
    }

    fflush(f);
    fclose(f);
    free(ents);
    fprintf(stderr, "QZcam: 录像恢复 %s —— 重建索引 %zu 帧\n", path, count);
    return true;
}

int qzcam_rec_repair_all(void)
{
    const char *dir_name = qzcam_storage_dir();
    DIR *dir = opendir(dir_name);
    if (!dir) return 0;

    int repaired = 0;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        const char *dot = strrchr(item->d_name, '.');
        if (!dot || strcasecmp(dot, ".avi") != 0) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir_name, item->d_name);

        /* 正在录的那个文件别动：边写边改会把它写坏 */
        pthread_mutex_lock(&lock);
        bool is_open = active && strcmp(full, file_path) == 0;
        pthread_mutex_unlock(&lock);
        if (is_open) continue;

        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 64) continue;
        if (repair_one(full)) repaired++;
    }
    closedir(dir);
    return repaired;
}
