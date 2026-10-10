#include "cam_encoder.h"

#if !defined(QZCAM_HAVE_MEDIA)

/* 模拟器 / 没带 Rockchip 媒体库的构建：整层是桩，录像自动落回 AVI。 */

bool qzcam_enc_available(void) { return false; }

bool qzcam_enc_open(const char *path, int w, int h, int vir_w, int vir_h,
                    int fps, int codec, int rc_mode, int bitrate_kbps)
{
    (void)path; (void)w; (void)h; (void)vir_w; (void)vir_h; (void)fps;
    (void)codec; (void)rc_mode; (void)bitrate_kbps;
    return false;
}

bool qzcam_enc_send(void *video_frame) { (void)video_frame; return false; }
void qzcam_enc_close(void) {}
bool qzcam_enc_active(void) { return false; }
int qzcam_enc_frames(void) { return 0; }
unsigned long long qzcam_enc_bytes(void) { return 0; }

const char *qzcam_enc_last_error(void)
{
    return "这个构建没带硬件编码器（VENC）";
}

#else /* ------------------------------ 真机实现 ------------------------------ */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rk_comm_venc.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_venc.h"
#include "rkmuxer.h"

#include "cam_isp.h"

#define ENC_CHN   0     /* VENC 通道号（与 VI 的通道号是两个命名空间） */
#define MUXER_ID  0     /* rkmuxer 实例号；同时只有一路录像，固定 0 */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t reader;
static bool reader_started;
static volatile bool running;
static bool active;
static int codec_is_h265;
static unsigned frame_total;
static unsigned long long byte_total;
static char error_text[160];

static void set_error(const char *text)
{
    pthread_mutex_lock(&lock);
    snprintf(error_text, sizeof(error_text), "%s", text);
    pthread_mutex_unlock(&lock);
}

/* 关键帧判定：IDR/I 帧要告诉封装器（MP4 的 stss 靠它，播放器 seek 才有锚点） */
static int is_keyframe(const VENC_PACK_S *pack)
{
    return pack->DataType.enH264EType == H264E_NALU_IDRSLICE ||
           pack->DataType.enH264EType == H264E_NALU_ISLICE ||
           pack->DataType.enH265EType == H265E_NALU_IDRSLICE ||
           pack->DataType.enH265EType == H265E_NALU_ISLICE;
}

/* 取流线程：VENC 一出码流就写进 MP4。写盘失败要立刻停 —— 否则码流缓冲写满，
 * 送帧那边会一直阻塞。 */
static void *reader_main(void *argument)
{
    (void)argument;
    VENC_STREAM_S stream;
    memset(&stream, 0, sizeof(stream));
    stream.pstPack = malloc(sizeof(VENC_PACK_S));   /* GNU 取流约定：调用方给槽位 */
    if (!stream.pstPack) {
        set_error("内存不足（编码取流缓冲）");
        pthread_mutex_lock(&lock);
        running = false;
        active = false;
        pthread_mutex_unlock(&lock);
        return NULL;
    }

    while (running) {
        memset(stream.pstPack, 0, sizeof(VENC_PACK_S));
        stream.u32PackCount = 1;
        if (RK_MPI_VENC_GetStream(ENC_CHN, &stream, 200) != RK_SUCCESS)
            continue;                                /* 超时：这一轮没帧 */

        for (RK_U32 i = 0; i < stream.u32PackCount; i++) {
            VENC_PACK_S *pack = &stream.pstPack[i];
            void *data = RK_MPI_MB_Handle2VirAddr(pack->pMbBlk);
            if (!data || pack->u32Len == 0) continue;

            if (rkmuxer_write_video_frame(MUXER_ID, (unsigned char *)data,
                                          pack->u32Len, (int64_t)pack->u64PTS,
                                          is_keyframe(pack)) != 0) {
                pthread_mutex_lock(&lock);
                snprintf(error_text, sizeof(error_text),
                         "写入失败（卡满/卡拔出？）编码录像已保存");
                running = false;
                active = false;
                pthread_mutex_unlock(&lock);
                break;
            }
            pthread_mutex_lock(&lock);
            frame_total++;
            byte_total += pack->u32Len;
            pthread_mutex_unlock(&lock);
        }
        RK_MPI_VENC_ReleaseStream(ENC_CHN, &stream);
    }

    free(stream.pstPack);
    return NULL;
}

/* 速率控制：CBR/VBR/AVBR 的公共字段（GOP 与帧率换算）都一样，只有码率字段不同 */
#define RC_COMMON(member)                                                      \
    do {                                                                       \
        (member).u32Gop = (RK_U32)(fps * 2);           /* 2 秒一个 I 帧 */      \
        (member).u32SrcFrameRateNum = (RK_U32)fps;                             \
        (member).u32SrcFrameRateDen = 1;                                       \
        (member).fr32DstFrameRateNum = (RK_U32)fps;                            \
        (member).fr32DstFrameRateDen = 1;                                      \
    } while (0)

bool qzcam_enc_available(void)
{
    /* 要有 ISP 的 NV12 帧可喂；没有就老实回去写 AVI */
    return qzcam_isp_ready();
}

bool qzcam_enc_open(const char *path, int w, int h, int vir_w, int vir_h,
                    int fps, int codec, int rc_mode, int bitrate_kbps)
{
    if (!path || w <= 0 || h <= 0 || fps <= 0) return false;
    if (vir_w < w) vir_w = w;
    if (vir_h < h) vir_h = h;
    if (bitrate_kbps < 32) bitrate_kbps = 32;

    pthread_mutex_lock(&lock);
    if (active) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    error_text[0] = '\0';
    codec_is_h265 = (codec == 0);
    frame_total = 0;
    byte_total = 0;
    pthread_mutex_unlock(&lock);

    VENC_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.stVencAttr.enType = codec_is_h265 ? RK_VIDEO_ID_HEVC : RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;          /* 直接吃 VI 的 NV12 */
    attr.stVencAttr.u32MaxPicWidth = (RK_U32)w;
    attr.stVencAttr.u32MaxPicHeight = (RK_U32)h;
    attr.stVencAttr.u32PicWidth = (RK_U32)w;
    attr.stVencAttr.u32PicHeight = (RK_U32)h;
    attr.stVencAttr.u32VirWidth = (RK_U32)vir_w;
    attr.stVencAttr.u32VirHeight = (RK_U32)vir_h;
    attr.stVencAttr.u32StreamBufCnt = 4;
    /* 码流缓冲：够放几帧就行（rkipc 默认 1.8MB，这里按分辨率减半给） */
    attr.stVencAttr.u32BufSize = (RK_U32)((long)w * h / 2);
    attr.stVencAttr.u32Profile = 100;                          /* H.264 High（H.265 不看它） */
    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;

    /* 码率控制：0=智能(AVBR，静止画面自动降码率，平均下来最省) 1=CBR 2=VBR */
    RK_U32 kbps = (RK_U32)bitrate_kbps;
    if (codec_is_h265) {
        if (rc_mode == 1) {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
            RC_COMMON(attr.stRcAttr.stH265Cbr);
            attr.stRcAttr.stH265Cbr.u32BitRate = kbps;
            attr.stRcAttr.stH265Cbr.u32StatTime = 1;
        } else if (rc_mode == 2) {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
            RC_COMMON(attr.stRcAttr.stH265Vbr);
            attr.stRcAttr.stH265Vbr.u32BitRate = kbps * 3 / 4;
            attr.stRcAttr.stH265Vbr.u32MaxBitRate = kbps * 3 / 2;
            attr.stRcAttr.stH265Vbr.u32MinBitRate = kbps / 4;
            attr.stRcAttr.stH265Vbr.u32StatTime = 1;
        } else {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H265AVBR;
            RC_COMMON(attr.stRcAttr.stH265Avbr);
            attr.stRcAttr.stH265Avbr.u32BitRate = kbps;
            attr.stRcAttr.stH265Avbr.u32MaxBitRate = kbps * 3 / 2;
            attr.stRcAttr.stH265Avbr.u32MinBitRate = kbps / 8;
            attr.stRcAttr.stH265Avbr.u32StatTime = 1;
        }
    } else {
        if (rc_mode == 1) {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
            RC_COMMON(attr.stRcAttr.stH264Cbr);
            attr.stRcAttr.stH264Cbr.u32BitRate = kbps;
            attr.stRcAttr.stH264Cbr.u32StatTime = 1;
        } else if (rc_mode == 2) {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
            RC_COMMON(attr.stRcAttr.stH264Vbr);
            attr.stRcAttr.stH264Vbr.u32BitRate = kbps * 3 / 4;
            attr.stRcAttr.stH264Vbr.u32MaxBitRate = kbps * 3 / 2;
            attr.stRcAttr.stH264Vbr.u32MinBitRate = kbps / 4;
            attr.stRcAttr.stH264Vbr.u32StatTime = 1;
        } else {
            attr.stRcAttr.enRcMode = VENC_RC_MODE_H264AVBR;
            RC_COMMON(attr.stRcAttr.stH264Avbr);
            attr.stRcAttr.stH264Avbr.u32BitRate = kbps;
            attr.stRcAttr.stH264Avbr.u32MaxBitRate = kbps * 3 / 2;
            attr.stRcAttr.stH264Avbr.u32MinBitRate = kbps / 8;
            attr.stRcAttr.stH264Avbr.u32StatTime = 1;
        }
    }

    if (RK_MPI_VENC_CreateChn(ENC_CHN, &attr) != RK_SUCCESS) {
        set_error("编码通道创建失败（分辨率或码率不支持）");
        return false;
    }

    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;                                   /* 不限帧数 */
    if (RK_MPI_VENC_StartRecvFrame(ENC_CHN, &recv) != RK_SUCCESS) {
        RK_MPI_VENC_DestroyChn(ENC_CHN);
        set_error("编码通道启动收帧失败");
        return false;
    }

    VideoParam video;
    memset(&video, 0, sizeof(video));
    snprintf(video.format, sizeof(video.format), "NV12");
    snprintf(video.codec, sizeof(video.codec), "%s", codec_is_h265 ? "H.265" : "H.264");
    video.width = w;
    video.height = h;
    video.vir_width = vir_w;
    video.vir_height = vir_h;
    video.bit_rate = bitrate_kbps * 1024;                      /* 单位是 bps */
    video.profile = 100;
    video.level = 52;
    video.frame_rate_den = 1;
    video.frame_rate_num = fps;

    AudioParam audio;                                          /* 不录音：空 codec → 只封视频轨 */
    memset(&audio, 0, sizeof(audio));

    if (rkmuxer_init(MUXER_ID, NULL, path, &video, &audio) != 0) {
        RK_MPI_VENC_StopRecvFrame(ENC_CHN);
        RK_MPI_VENC_DestroyChn(ENC_CHN);
        set_error("MP4 文件创建失败（目录不可写？）");
        return false;
    }

    pthread_mutex_lock(&lock);
    running = true;
    pthread_mutex_unlock(&lock);

    if (pthread_create(&reader, NULL, reader_main, NULL) != 0) {
        running = false;
        rkmuxer_deinit(MUXER_ID);
        RK_MPI_VENC_StopRecvFrame(ENC_CHN);
        RK_MPI_VENC_DestroyChn(ENC_CHN);
        set_error("创建编码取流线程失败");
        return false;
    }
    reader_started = true;

    pthread_mutex_lock(&lock);
    active = true;
    pthread_mutex_unlock(&lock);

    fprintf(stderr, "QZcam: 硬编码录像 %s %s %dx%d@%dfps %dkbps\n",
            path, codec_is_h265 ? "H.265" : "H.264", w, h, fps, bitrate_kbps);
    return true;
}

bool qzcam_enc_send(void *video_frame)
{
    if (!video_frame) return false;

    pthread_mutex_lock(&lock);
    bool ok = active && running;
    pthread_mutex_unlock(&lock);
    if (!ok) return false;

    /* 这里不持锁：SendFrame 可能阻塞（编码器忙），持锁会把取流线程也卡住，
     * 形成互相等待。调用方（cam_record）用自己那把锁保证 send 与 close 不并发。 */
    return RK_MPI_VENC_SendFrame(ENC_CHN, (const VIDEO_FRAME_INFO_S *)video_frame,
                                 1000) == RK_SUCCESS;
}

void qzcam_enc_close(void)
{
    pthread_mutex_lock(&lock);
    bool started = reader_started;
    if (!active && !started) {
        pthread_mutex_unlock(&lock);
        return;
    }
    active = false;
    running = false;
    reader_started = false;
    pthread_mutex_unlock(&lock);

    if (started) pthread_join(reader, NULL);   /* 先让取流线程收笔，再落 moov */

    RK_MPI_VENC_StopRecvFrame(ENC_CHN);
    RK_MPI_VENC_DestroyChn(ENC_CHN);
    rkmuxer_deinit(MUXER_ID);
}

bool qzcam_enc_active(void)
{
    pthread_mutex_lock(&lock);
    bool value = active;
    pthread_mutex_unlock(&lock);
    return value;
}

int qzcam_enc_frames(void)
{
    pthread_mutex_lock(&lock);
    int value = (int)frame_total;
    pthread_mutex_unlock(&lock);
    return value;
}

unsigned long long qzcam_enc_bytes(void)
{
    pthread_mutex_lock(&lock);
    unsigned long long value = byte_total;
    pthread_mutex_unlock(&lock);
    return value;
}

const char *qzcam_enc_last_error(void)
{
    return error_text[0] ? error_text : "";
}

#endif /* !QZCAM_HAVE_MEDIA */
