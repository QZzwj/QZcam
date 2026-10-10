/* cam_player —— 板载录像回放
 *
 * 两种后端（按文件头自动分）：
 *   AVI（自家写的未压缩 RGB565）：直接读块 + 翻行，零解码 —— 最快，也最省电。
 *   MP4（硬件编码的 H.264/H.265）：rkdemuxer 拆包 → MPI VDEC 解码 → 最近邻缩到
 *   面板尺寸的 RGB565。这条路只在带媒体库的真机构建里存在；模拟器里 MP4 打不开，
 *   界面会给一句提示。
 *
 * 对外接口两种后端一致：open / next_frame / seek_frame / total / fps / w / h。
 */
#include "cam_player.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "qzcam.h"

#if defined(QZCAM_HAVE_MEDIA)
#include "rk_comm_vdec.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vdec.h"
#include "rkdemuxer.h"
#endif

enum { PLAYER_NONE = 0, PLAYER_AVI = 1, PLAYER_MP4 = 2 };
static int kind;

/* ================================ AVI 后端 ================================ */

static FILE *fp;
static long movi_data;        /* 'movi' fourcc 之后的绝对文件位置 */
static long *frame_pos;       /* 每帧 chunk 头的绝对位置 */
static unsigned *frame_size;
static int frame_total;
static int cursor;
static int width, height, fps;
static uint8_t *raw;          /* 读入的原始块（底上行） */
static uint8_t *out;          /* 翻转后的自上而下帧 */

static void avi_close(void)
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

static bool avi_open(const char *path)
{
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
            unsigned char kind4[4];
            if (fread(kind4, 1, 4, fp) != 4) break;
            if (!memcmp(kind4, "movi", 4)) {
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
        avi_close();
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
        avi_close();
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
        avi_close();
        return false;
    }
    kind = PLAYER_AVI;
    cursor = 0;
    return true;
}

static const uint8_t *avi_next_frame(void)
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

/* ================================ MP4 后端 ================================ */

/* 这几个状态两种构建都要有（对外取值会碰它们），只是桩构建里永远是初值 */
static int mp4_fps = 15;
static int mp4_frames_total;
static int mp4_index;

#if defined(QZCAM_HAVE_MEDIA)

#define PLAYER_VDEC_CHN 0

static void *mp4_demuxer;
static bool mp4_vdec;
static int mp4_w, mp4_h;
static bool mp4_eos;
static uint8_t *mp4_rgb;                 /* 缩到面板尺寸的 RGB565 */
static char player_error[128];

/* RV1106（以及 RV1103）**只有编码器没有解码器**：设备树里只有 ffa50000.rkvenc，
 * 没有 rkvdec。而 rockit 的 VDEC 接口在缺解码硬件时不会老实报错 ——
 * RK_MPI_VDEC_CreateChn 返回成功，下一个 ioctl 在内核 mpp_vcodec 里查不到通道，
 * 空指针 Oops（"Internal error: Oops - BUG: 17 ... mpp_vcodec_chan_control"），
 * 而且那个 ioctl **永远不返回**，直接把调用它的线程（界面线程）挂死。
 *
 * 所以先看平台有没有解码硬件的迹象，没有就根本不碰 VDEC。 */
static bool vdec_hardware_present(void)
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    DIR *dir = opendir("/sys/devices/platform");
    if (dir) {
        struct dirent *item;
        while ((item = readdir(dir)) != NULL) {
            if (strstr(item->d_name, "rkvdec") || strstr(item->d_name, "-vdec")) {
                cached = 1;
                break;
            }
        }
        closedir(dir);
    }
    if (!cached)
        fprintf(stderr, "QZcam: 本平台没有视频解码器（/sys/devices/platform 里没有 rkvdec），"
                        "板上 MP4 回放不可用\n");
    return cached != 0;
}

/* 通道参数：重启解码（seek）时要原样再建一次，所以留着 */
static RK_CODEC_ID_E mp4_codec;
static RK_U32 mp4_vir_w, mp4_vir_h, mp4_frame_buf_size;

static inline uint16_t yuv_to_rgb565(int y, int u, int v)
{
    int r = y + ((359 * (v - 128)) >> 8);
    int g = y - ((88 * (u - 128) + 183 * (v - 128)) >> 8);
    int b = y + ((454 * (u - 128)) >> 8);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* NV12 → 面板尺寸 RGB565（最近邻，和取景那套同一个思路：整数乘除，不做插值） */
static void mp4_convert(const uint8_t *nv12, int src_w, int src_h, int stride, int vir_h)
{
    int pw = qzcam_panel_w, ph = qzcam_panel_h;
    if (pw <= 0 || ph <= 0) return;
    const uint8_t *y_plane = nv12;
    const uint8_t *uv_plane = nv12 + (size_t)stride * (size_t)(vir_h ? vir_h : src_h);
    for (int y = 0; y < ph; y++) {
        int sy = y * src_h / ph;
        const uint8_t *yrow = y_plane + (size_t)sy * (size_t)stride;
        const uint8_t *uvrow = uv_plane + (size_t)(sy / 2) * (size_t)stride;
        uint16_t *orow = (uint16_t *)(mp4_rgb + (size_t)y * (size_t)pw * 2);
        for (int x = 0; x < pw; x++) {
            int sx = x * src_w / pw;
            orow[x] = yuv_to_rgb565(yrow[sx], uvrow[sx & ~1], uvrow[(sx & ~1) + 1]);
        }
    }
}

static void mp4_vdec_down(void)
{
    if (!mp4_vdec) return;
    RK_MPI_VDEC_StopRecvStream(PLAYER_VDEC_CHN);
    RK_MPI_VDEC_DestroyChn(PLAYER_VDEC_CHN);
    mp4_vdec = false;
}

/* 建解码通道并从 start_ms 开始送包。seek 也走它（先 down 再 up，状态最干净）。 */
static bool mp4_vdec_up(long long start_ms)
{
    VDEC_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.enMode = VIDEO_MODE_FRAME;
    attr.enType = mp4_codec;
    attr.u32PicWidth = (RK_U32)mp4_w;
    attr.u32PicHeight = (RK_U32)mp4_h;
    attr.u32PicVirWidth = mp4_vir_w;
    attr.u32PicVirHeight = mp4_vir_h;
    attr.u32FrameBufCnt = 5;                 /* 留够重排余量 */
    attr.u32StreamBufCnt = 4;
    attr.u32FrameBufSize = mp4_frame_buf_size;
    attr.stVdecVideoAttr.bTemporalMvpEnable = RK_FALSE;
    if (RK_MPI_VDEC_CreateChn(PLAYER_VDEC_CHN, &attr) != RK_SUCCESS) return false;

    VDEC_CHN_PARAM_S param;
    memset(&param, 0, sizeof(param));
    param.stVdecVideoParam.enCompressMode = COMPRESS_MODE_NONE;
    RK_MPI_VDEC_SetChnParam(PLAYER_VDEC_CHN, &param);      /* 失败不致命 */

    if (RK_MPI_VDEC_StartRecvStream(PLAYER_VDEC_CHN) != RK_SUCCESS) {
        RK_MPI_VDEC_DestroyChn(PLAYER_VDEC_CHN);
        return false;
    }
    mp4_vdec = true;

    if (rkdemuxer_read_packet_start(mp4_demuxer, start_ms) != 0) {
        mp4_vdec_down();
        return false;
    }
    mp4_eos = false;
    mp4_index = (int)(start_ms * (mp4_fps > 0 ? mp4_fps : 15) / 1000);
    return true;
}

/* 读一个包 → 包成 MB → 送解码。包数据由 demuxer malloc，我们负责 free。 */
static bool mp4_feed_packet(void)
{
    DemuxerPacket packet;
    memset(&packet, 0, sizeof(packet));
    if (rkdemuxer_read_one_video_packet(mp4_demuxer, &packet) != 0) return false;
    if (packet.s8EofFlag || !packet.s8PacketData || packet.s32PacketSize <= 0) {
        free(packet.s8PacketData);
        return false;
    }

    MB_EXT_CONFIG_S config;
    memset(&config, 0, sizeof(config));
    config.pu8VirAddr = (RK_U8 *)packet.s8PacketData;
    config.u64Size = (RK_U64)packet.s32PacketSize;
    config.pOpaque = packet.s8PacketData;    /* 不设 pFreeCB：下面自己 free */

    MB_BLK blk = NULL;
    if (RK_MPI_SYS_CreateMB(&blk, &config) != RK_SUCCESS) {
        free(packet.s8PacketData);
        return false;
    }

    VDEC_STREAM_S stream;
    memset(&stream, 0, sizeof(stream));
    stream.pMbBlk = blk;
    stream.u32Len = (RK_U32)packet.s32PacketSize;
    stream.u64PTS = (RK_U64)packet.s64Pts;
    stream.bBypassMbBlk = RK_TRUE;           /* 用我们给的地址，别再拷一份 */
    RK_S32 ret = RK_MPI_VDEC_SendStream(PLAYER_VDEC_CHN, &stream, -1);
    RK_MPI_MB_ReleaseMB(blk);
    free(packet.s8PacketData);
    return ret == RK_SUCCESS;
}

static void mp4_close(void)
{
    mp4_vdec_down();
    if (mp4_demuxer) {
        rkdemuxer_deinit(&mp4_demuxer);
        mp4_demuxer = NULL;
    }
    free(mp4_rgb);
    mp4_rgb = NULL;
    mp4_index = 0;
    mp4_frames_total = 0;
}

static bool mp4_open(const char *path)
{
    player_error[0] = '\0';

    /* 没有解码硬件就别往下走：见 vdec_hardware_present() 的说明 —— 硬走会把内核
     * 打 Oops 且 ioctl 永不返回。 */
    if (!vdec_hardware_present()) {
        snprintf(player_error, sizeof(player_error),
                 "本机没有视频解码器，MP4 请拷到电脑或手机上看");
        return false;
    }

    DemuxerInput input;
    memset(&input, 0, sizeof(input));
    input.ptr = NULL;
    input.s8ReadModeFlag = 1;          /* ACTIVE：自己拉包（read_one_video_packet） */
    input.s8VideoEnableFlag = 1;
    input.s8AudioEnableFlag = 0;
    input.transport = NULL;
    input.rtsp_io_timeout = 0;
    if (rkdemuxer_init(&mp4_demuxer, &input) != 0) {
        mp4_demuxer = NULL;
        fprintf(stderr, "QZcam: MP4 打不开：rkdemuxer 初始化失败\n");
        snprintf(player_error, sizeof(player_error), "打不开这个 MP4（解封装失败）");
        return false;
    }

    DemuxerParam param;
    memset(&param, 0, sizeof(param));
    param.pstReadPacketCallback.read_video_packet = NULL;
    param.pstReadPacketCallback.read_audio_packet = NULL;
    if (rkdemuxer_get_param(mp4_demuxer, path, &param) != 0) {
        fprintf(stderr, "QZcam: MP4 打不开：解析文件参数失败（不是 mp4？）%s\n", path);
        mp4_close();
        return false;
    }

    mp4_w = param.s32VideoWidth;
    mp4_h = param.s32VideoHeigh;
    mp4_fps = param.s32VideoAvgFrameRate > 0 ? param.s32VideoAvgFrameRate : 15;
    if (mp4_fps > 75) mp4_fps = 75;
    if (mp4_w <= 0 || mp4_h <= 0) {
        fprintf(stderr, "QZcam: MP4 打不开：拿到的画面尺寸无效 %dx%d\n", mp4_w, mp4_h);
        mp4_close();
        return false;
    }

    mp4_codec = RK_VIDEO_ID_AVC;
    if (param.pVideoCodec && strstr((const char *)param.pVideoCodec, "265"))
        mp4_codec = RK_VIDEO_ID_HEVC;

    /* 时长：进度条与总帧数都靠它（拿不到就按 0 处理，界面只显示已播） */
    long long duration = 0;
    if (rkdemuxer_read_video_duration(mp4_demuxer, &duration) != 0 || duration <= 0)
        duration = param.s32TotalTime;
    mp4_frames_total = (int)(duration * mp4_fps / 1000);
    if (mp4_frames_total < 1) mp4_frames_total = 1;

    VDEC_PIC_BUF_ATTR_S picbuf;
    memset(&picbuf, 0, sizeof(picbuf));
    picbuf.enCodecType = mp4_codec;
    picbuf.stPicBufAttr.u32Width = (RK_U32)mp4_w;
    picbuf.stPicBufAttr.u32Height = (RK_U32)mp4_h;
    picbuf.stPicBufAttr.enPixelFormat = RK_FMT_YUV420SP;
    picbuf.stPicBufAttr.enCompMode = COMPRESS_MODE_NONE;
    MB_PIC_CAL_S cal;
    memset(&cal, 0, sizeof(cal));
    if (RK_MPI_CAL_VDEC_GetPicBufferSize(&picbuf, &cal) != RK_SUCCESS) {
        fprintf(stderr, "QZcam: MP4 打不开：算不出解码缓冲大小（%dx%d codec=%d）\n",
                mp4_w, mp4_h, (int)mp4_codec);
        mp4_close();
        return false;
    }
    mp4_vir_w = cal.u32VirWidth ? cal.u32VirWidth : (RK_U32)mp4_w;
    mp4_vir_h = cal.u32VirHeight ? cal.u32VirHeight : (RK_U32)mp4_h;
    mp4_frame_buf_size = cal.u32MBSize;

    mp4_rgb = malloc((size_t)qzcam_panel_w * (size_t)qzcam_panel_h * 2);
    if (!mp4_rgb) {
        fprintf(stderr, "QZcam: MP4 打不开：没有内存放下转换缓冲（%dx%d RGB565）\n",
                qzcam_panel_w, qzcam_panel_h);
        mp4_close();
        return false;
    }
    if (!mp4_vdec_up(0)) {
        fprintf(stderr, "QZcam: MP4 打不开：VDEC 建通道/起送包失败（%dx%d codec=%d）\n",
                mp4_w, mp4_h, (int)mp4_codec);
        mp4_close();
        return false;
    }

    kind = PLAYER_MP4;
    return true;
}

static const uint8_t *mp4_next_frame(void)
{
    if (!mp4_vdec || !mp4_rgb) return NULL;

    for (int guard = 0; guard < 10000; guard++) {
        VIDEO_FRAME_INFO_S frame;
        memset(&frame, 0, sizeof(frame));
        /* 拆包到头后给解码器一点排空时间，否则最后一两帧会被吞掉 */
        if (RK_MPI_VDEC_GetFrame(PLAYER_VDEC_CHN, &frame, mp4_eos ? 300 : 0) == RK_SUCCESS) {
            const uint8_t *nv12 =
                (const uint8_t *)RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
            const uint8_t *result = NULL;
            if (nv12) {
                mp4_convert(nv12, (int)frame.stVFrame.u32Width, (int)frame.stVFrame.u32Height,
                            (int)frame.stVFrame.u32VirWidth, (int)frame.stVFrame.u32VirHeight);
                result = mp4_rgb;
            }
            RK_MPI_VDEC_ReleaseFrame(PLAYER_VDEC_CHN, &frame);
            mp4_index++;
            return result;
        }
        if (mp4_eos) return NULL;                  /* 解码器也空了 = 真的播完 */
        if (!mp4_feed_packet()) {
            mp4_eos = true;                        /* 拆包到头，转去排空 */
            continue;
        }
    }
    return NULL;
}

static void mp4_seek(int n)
{
    if (!mp4_demuxer) return;
    if (n < 0) n = 0;
    if (mp4_frames_total > 0 && n > mp4_frames_total) n = mp4_frames_total;
    long long ms = (long long)n * 1000 / (mp4_fps > 0 ? mp4_fps : 15);
    mp4_vdec_down();
    mp4_vdec_up(ms);
}

#else  /* 没有媒体库：MP4 后端是桩 */

static bool mp4_open(const char *path) { (void)path; return false; }
static void mp4_close(void) {}
static const uint8_t *mp4_next_frame(void) { return NULL; }
static void mp4_seek(int n) { (void)n; }

#endif /* QZCAM_HAVE_MEDIA */

/* ================================= 对外 ================================= */

bool qzcam_player_open(const char *path)
{
    qzcam_player_close();
    if (!path) return false;

    /* 按文件头分后端：RIFF/AVI 是自家未压缩录像，其它（ftyp mp4）走解码 */
    FILE *probe = fopen(path, "rb");
    if (!probe) return false;
    unsigned char magic[12] = {0};
    size_t got = fread(magic, 1, 12, probe);
    fclose(probe);

    if (got == 12 && !memcmp(magic, "RIFF", 4)) return avi_open(path);
    return mp4_open(path);
}

const uint8_t *qzcam_player_next_frame(void)
{
    if (kind == PLAYER_AVI) return avi_next_frame();
    if (kind == PLAYER_MP4) return mp4_next_frame();
    return NULL;
}

void qzcam_player_seek_frame(int n)
{
    if (n < 0) n = 0;
    if (kind == PLAYER_AVI) {
        if (n > frame_total) n = frame_total;
        cursor = n;
    } else if (kind == PLAYER_MP4) {
        mp4_seek(n);
    }
}

int qzcam_player_frame_index(void)
{
    if (kind == PLAYER_AVI) return fp ? cursor : 0;
    if (kind == PLAYER_MP4) return mp4_index;
    return 0;
}

int qzcam_player_total_frames(void)
{
    if (kind == PLAYER_AVI) return fp ? frame_total : 0;
    if (kind == PLAYER_MP4) return mp4_frames_total;
    return 0;
}

int qzcam_player_frame_w(void)
{
    if (kind == PLAYER_AVI) return width;
    return kind == PLAYER_MP4 ? qzcam_panel_w : 0;
}

int qzcam_player_frame_h(void)
{
    if (kind == PLAYER_AVI) return height;
    return kind == PLAYER_MP4 ? qzcam_panel_h : 0;
}

int qzcam_player_fps(void)
{
    if (kind == PLAYER_AVI) return fps;
    if (kind == PLAYER_MP4) return mp4_fps;
    return 15;
}

void qzcam_player_close(void)
{
    if (kind == PLAYER_AVI) avi_close();
    else if (kind == PLAYER_MP4) mp4_close();
    kind = PLAYER_NONE;
}

const char *qzcam_player_last_error(void)
{
#if defined(QZCAM_HAVE_MEDIA)
    return player_error[0] ? player_error : "";
#else
    return "";
#endif
}
