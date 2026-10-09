/* cam_player —— 板载录像回放解析器
 *
 * 只解析 cam_record 写出的那一种 AVI：未压缩 RGB565（BI_BITFIELDS 565），
 * 帧块 fourcc 00dc/00db，底上行（bottom-up）行序，idx1 索引（相对 'movi'
 * 数据起点的标准偏移）。正因为不用解任何编码，板上播放就是"读块+翻行"。
 */
#include "cam_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *fp;
static long movi_data;        /* 'movi' fourcc 之后的绝对文件位置 */
static long *frame_pos;       /* 每帧 chunk 头的绝对位置 */
static unsigned *frame_size;
static int frame_total;
static int cursor;
static int width, height, fps;
static uint8_t *raw;          /* 读入的原始块（底上行） */
static uint8_t *out;          /* 翻转后的自上而下帧 */

bool qzcam_player_open(const char *path)
{
    qzcam_player_close();
    fp = fopen(path, "rb");
    if (!fp) return false;

    /* ---- 走一遍顶层块：RIFF....AVI [LIST hdrl] [LIST movi] [idx1] ---- */
    unsigned char head[12];
    if (fread(head, 1, 12, fp) != 12 || memcmp(head, "RIFF", 4) ||
        memcmp(head + 8, "AVI ", 4)) {
        fclose(fp);
        fp = NULL;
        return false;
    }

    long movi_list = 0, idx_pos = 0, idx_size = 0;
    long pos = 12;
    while (1) {
        unsigned char ck[8];
        if (fseek(fp, pos, SEEK_SET) != 0 || fread(ck, 1, 8, fp) != 8) break;
        unsigned sz = (unsigned)ck[4] | ((unsigned)ck[5] << 8) |
                      ((unsigned)ck[6] << 16) | ((unsigned)ck[7] << 24);
        if (!memcmp(ck, "LIST", 4)) {
            unsigned char kind[4];
            if (fread(kind, 1, 4, fp) != 4) break;
            if (!memcmp(kind, "movi", 4)) {
                movi_list = pos;
                movi_data = pos + 8;         /* 'movi' fourcc 之后 */
            }
            pos += 12;                       /* 进 LIST 内部继续扫 */
            continue;
        }
        if (!memcmp(ck, "idx1", 4)) {
            idx_pos = pos + 8;
            idx_size = sz;
            break;
        }
        pos += 8 + (long)((sz + 1) & ~1u);   /* 块按字对齐 */
    }
    if (!movi_list) {
        fclose(fp);
        fp = NULL;
        return false;
    }

    /* ---- 索引：每项 16 字节 {ckid, dwFlags, dwOffset, dwSize} ---- */
    int count = (int)(idx_size / 16);
    if (count <= 0) {
        fclose(fp);
        fp = NULL;
        return false;
    }
    frame_pos = malloc(sizeof(long) * (size_t)count);
    frame_size = malloc(sizeof(unsigned) * (size_t)count);
    frame_total = 0;
    if (fseek(fp, idx_pos, SEEK_SET) != 0) {
        free(frame_pos);
        free(frame_size);
        fclose(fp);
        fp = NULL;
        return false;
    }
    for (int i = 0; i < count; i++) {
        unsigned char e[16];
        if (fread(e, 1, 16, fp) != 16) break;
        bool video = !memcmp(e, "00dc", 4) || !memcmp(e, "00db", 4);
        if (!video) continue;
        unsigned off = (unsigned)e[8] | ((unsigned)e[9] << 8) |
                       ((unsigned)e[10] << 16) | ((unsigned)e[11] << 24);
        unsigned sz = (unsigned)e[12] | ((unsigned)e[13] << 8) |
                      ((unsigned)e[14] << 16) | ((unsigned)e[15] << 24);
        frame_pos[frame_total] = movi_data + (long)off;
        frame_size[frame_total] = sz;
        frame_total++;
    }
    if (frame_total <= 0) {
        free(frame_pos);
        free(frame_size);
        fclose(fp);
        fp = NULL;
        return false;
    }

    /* ---- 元数据：hdrl 里抓 avih(宽高) 与 strh(帧率) ---- */
    width = 320;
    height = 240;
    fps = 15;
    long scan = 12;
    while (scan < movi_list) {
        unsigned char ck[8];
        if (fseek(fp, scan, SEEK_SET) != 0 || fread(ck, 1, 8, fp) != 8) break;
        unsigned sz = (unsigned)ck[4] | ((unsigned)ck[5] << 8) |
                      ((unsigned)ck[6] << 16) | ((unsigned)ck[7] << 24);
        if (!memcmp(ck, "avih", 4) && sz >= 40) {
            unsigned char a[40];
            if (fread(a, 1, 40, fp) == 40) {
                width = (int)((unsigned)a[32] | ((unsigned)a[33] << 8) |
                              ((unsigned)a[34] << 16) | ((unsigned)a[35] << 24));
                height = (int)((unsigned)a[36] | ((unsigned)a[37] << 8) |
                               ((unsigned)a[38] << 16) | ((unsigned)a[39] << 24));
            }
        } else if (!memcmp(ck, "strh", 4) && sz >= 28) {
            unsigned char s[28];
            if (fread(s, 1, 28, fp) == 28) {
                unsigned scale = (unsigned)s[20] | ((unsigned)s[21] << 8) |
                                 ((unsigned)s[22] << 16) | ((unsigned)s[23] << 24);
                unsigned rate = (unsigned)s[24] | ((unsigned)s[25] << 8) |
                                ((unsigned)s[26] << 16) | ((unsigned)s[27] << 24);
                if (scale) fps = (int)(rate / scale);
            }
        }
        scan += 8 + (long)((sz + 1) & ~1u);
    }
    if (width <= 0 || height <= 0 || width > 1280 || height > 1280) {
        width = 320;
        height = 240;
    }
    if (fps <= 0 || fps > 75) fps = 15;

    raw = malloc((size_t)width * height * 2);
    out = malloc((size_t)width * height * 2);
    if (!raw || !out) {
        qzcam_player_close();
        return false;
    }
    cursor = 0;
    return true;
}

const uint8_t *qzcam_player_next_frame(void)
{
    if (!fp || cursor >= frame_total) return NULL;
    if (fseek(fp, frame_pos[cursor] + 8, SEEK_SET) != 0 ||
        fread(raw, 1, frame_size[cursor], fp) != frame_size[cursor]) {
        cursor = frame_total;                /* 读坏了就当播完 */
        return NULL;
    }
    /* 底上行 → 自上而下 */
    int row_bytes = width * 2;
    for (int y = 0; y < height; y++)
        memcpy(out + (size_t)y * row_bytes,
               raw + (size_t)(height - 1 - y) * row_bytes, (size_t)row_bytes);
    cursor++;
    return out;
}

void qzcam_player_seek_frame(int n)
{
    if (n < 0) n = 0;
    if (n > frame_total) n = frame_total;
    cursor = n;
}

int qzcam_player_frame_index(void) { return fp ? cursor : 0; }
int qzcam_player_total_frames(void) { return fp ? frame_total : 0; }
int qzcam_player_frame_w(void) { return width; }
int qzcam_player_frame_h(void) { return height; }
int qzcam_player_fps(void) { return fps; }

void qzcam_player_close(void)
{
    if (fp) fclose(fp);
    fp = NULL;
    free(frame_pos);
    frame_pos = NULL;
    free(frame_size);
    frame_size = NULL;
    free(raw);
    raw = NULL;
    free(out);
    out = NULL;
    frame_total = 0;
    cursor = 0;
}
