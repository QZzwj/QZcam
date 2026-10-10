/* Rockchip ISP（rkaiq 3A）+ RKMPI VI 取景后端。
 *
 * 取材自 SDK 官方示例与 Echo-Mate 的实战用法：
 *   - ISP 初始化顺序：SDK/media/samples/example/common/isp3.x/sample_comm_isp.c
 *       enumStaticMetasByPhyId → preInit_devBufCnt → sysctl_init
 *       → sysctl_prepare → sysctl_start      （SAMPLE_COMM_ISP_Init/Run 的内部）
 *   - VI 设备/通道初始化：Echo-Mate rkmpi_demos .../src/luckfox_mpi.cc
 *   - 取帧：RK_MPI_VI_GetChnFrame + RK_MPI_MB_Handle2VirAddr（NV12）
 *
 * 与示例的差别：这里不引 libsample_comm，直接调 rkaiq 的 uAPI2 —— 少一层依赖，
 * 也更清楚每一步在做什么。 */

#include "cam_isp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(QZCAM_HAVE_MEDIA)

/* 没有 Rockchip 媒体库（开发机模拟器构建，或没给 QZCAM_MEDIA_DIR）：全桩。
 * open() 返回 false，cam_core 会落到别的后端，界面与存图链路照样能验证。 */
bool qzcam_isp_open(int width, int height)
{
    (void)width;
    (void)height;
    return false;
}

const uint8_t *qzcam_isp_grab(int *width, int *height, int *stride, int timeout_ms)
{
    (void)width;
    (void)height;
    (void)stride;
    (void)timeout_ms;
    return NULL;
}

void qzcam_isp_release(void) {}
void qzcam_isp_close(void) {}
int qzcam_isp_virtual_height(void) { return 0; }
void qzcam_isp_apply_settings(void) {}
void *qzcam_isp_held_frame(void) { return NULL; }
bool qzcam_isp_ready(void) { return false; }
void qzcam_isp_apply_ev(int ev_x10) { (void)ev_x10; }

const char *qzcam_isp_last_error(void)
{
    return "这个构建没带 rkaiq / RKMPI（真机固件才走 ISP 取景）";
}

#else /* ------------------------------ 真机实现 ------------------------------ */

#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rkaiq/uAPI2/rk_aiq_user_api2_sysctl.h"
#include "rkaiq/uAPI2/rk_aiq_user_api2_imgproc.h"
#include "rkaiq/uAPI2/rk_aiq_user_api2_ae.h"
#include "qzcam.h"
#include <math.h>

#define ISP_CAM_ID   0
#define ISP_VI_DEV   0
#define ISP_VI_PIPE  0
#define ISP_VI_CHN   0

static rk_aiq_sys_ctx_t *aiq_ctx;
static VIDEO_FRAME_INFO_S held_frame;
static bool frame_held;
static bool mpi_inited;
static bool vi_inited;
static char last_error[224];
static qzcam_settings_t applied_settings;
static bool settings_applied;

static void set_error(const char *text)
{
    snprintf(last_error, sizeof(last_error), "%s", text);
}

/* rkaiq 的回调：出错时只记一行，别打断取流（3A 会自己恢复） */
static XCamReturn isp_error_cb(rk_aiq_err_msg_t *msg)
{
    if (msg && msg->err_code != XCAM_RETURN_NO_ERROR)
        fprintf(stderr, "QZcam: rkaiq 报错 err_code=%d\n", msg->err_code);
    return XCAM_RETURN_NO_ERROR;
}

static XCamReturn isp_metas_cb(rk_aiq_metas_t *metas)
{
    (void)metas;
    return XCAM_RETURN_NO_ERROR;
}

/* VI 设备：取默认属性 → 使能 → 绑定 pipe（照 Echo-Mate luckfox_mpi.cc 的写法） */
static int vi_dev_init(void)
{
    VI_DEV_ATTR_S dev_attr;
    VI_DEV_BIND_PIPE_S bind_pipe;
    memset(&dev_attr, 0, sizeof(dev_attr));
    memset(&bind_pipe, 0, sizeof(bind_pipe));

    if (RK_MPI_VI_GetDevAttr(ISP_VI_DEV, &dev_attr) == RK_ERR_VI_NOT_CONFIG) {
        if (RK_MPI_VI_SetDevAttr(ISP_VI_DEV, &dev_attr) != RK_SUCCESS) return -1;
    }

    if (RK_MPI_VI_GetDevIsEnable(ISP_VI_DEV) != RK_SUCCESS) {
        if (RK_MPI_VI_EnableDev(ISP_VI_DEV) != RK_SUCCESS) return -1;
        bind_pipe.u32Num = 1;
        bind_pipe.PipeId[0] = ISP_VI_PIPE;
        if (RK_MPI_VI_SetDevBindPipe(ISP_VI_DEV, &bind_pipe) != RK_SUCCESS) return -1;
    }
    return 0;
}

/* VI 通道：NV12 + DMA buffer（ISP 输出的原生格式，转换交给我们自己做） */
static int vi_chn_init(int channel, int width, int height)
{
    VI_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.stIspOpt.u32BufCount = 2;
    attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    attr.stSize.u32Width = (RK_U32)width;
    attr.stSize.u32Height = (RK_U32)height;
    attr.enPixelFormat = RK_FMT_YUV420SP;   /* NV12 */
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.u32Depth = 2;

    if (RK_MPI_VI_SetChnAttr(ISP_VI_PIPE, channel, &attr) != RK_SUCCESS) return -1;
    if (RK_MPI_VI_EnableChn(ISP_VI_PIPE, channel) != RK_SUCCESS) return -1;
    return 0;
}

bool qzcam_isp_open(int width, int height)
{
    if (aiq_ctx) return true;
    last_error[0] = '\0';

    const char *iq_dir = getenv("QZCAM_IQDIR");
    if (!iq_dir || !iq_dir[0]) iq_dir = "/etc/iqfiles";

    /* NORMAL（非 HDR）模式必须在 init 之前设进去，与 SDK 示例一致 */
    setenv("HDR_MODE", "0", 1);

    /* 1) 传感器信息：能枚举到才说明驱动/模块/I2C 都通了 */
    rk_aiq_static_info_t static_info;
    memset(&static_info, 0, sizeof(static_info));
    if (rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(ISP_CAM_ID, &static_info) !=
            XCAM_RETURN_NO_ERROR ||
        static_info.sensor_info.sensor_name[0] == '\0') {
        set_error("找不到摄像头（传感器没 probe：模块没加载或 I2C 不通）");
        return false;
    }
    fprintf(stderr, "QZcam: ISP 传感器 %s，IQ 目录 %s\n",
            static_info.sensor_info.sensor_name, iq_dir);

    /* 2) rkaiq：init → prepare → start（3A 跑起来，图才正常） */
    rk_aiq_uapi2_sysctl_preInit_devBufCnt(static_info.sensor_info.sensor_name,
                                          "rkraw_rx", 4);
    aiq_ctx = rk_aiq_uapi2_sysctl_init(static_info.sensor_info.sensor_name, iq_dir,
                                      isp_error_cb, isp_metas_cb);
    if (!aiq_ctx) {
        set_error("rkaiq 初始化失败（IQ 文件缺失或目录不对）");
        return false;
    }
    if (rk_aiq_uapi2_sysctl_prepare(aiq_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) !=
        XCAM_RETURN_NO_ERROR) {
        set_error("rkaiq prepare 失败（sensor 与 IQ 不匹配？）");
        rk_aiq_uapi2_sysctl_deinit(aiq_ctx);
        aiq_ctx = NULL;
        return false;
    }
    if (rk_aiq_uapi2_sysctl_start(aiq_ctx) != XCAM_RETURN_NO_ERROR) {
        set_error("rkaiq start 失败（3A 起不来）");
        rk_aiq_uapi2_sysctl_deinit(aiq_ctx);
        aiq_ctx = NULL;
        return false;
    }
    fprintf(stderr, "QZcam: rkaiq 3A 已启动\n");

    /* 3) MPI + VI：取流（这一步需要 rockit.ko 已 insmod） */
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        set_error("RK_MPI_SYS_Init 失败（rockit.ko 没加载？）");
        goto fail_isp;
    }
    mpi_inited = true;

    if (vi_dev_init() != 0) {
        set_error("VI 设备初始化失败");
        goto fail_mpi;
    }
    if (vi_chn_init(ISP_VI_CHN, width, height) != 0) {
        set_error("VI 通道初始化失败（尺寸或带宽不支持？）");
        goto fail_mpi;
    }
    vi_inited = true;

    fprintf(stderr, "QZcam: VI %dx%d NV12 已开流\n", width, height);
    settings_applied = false;
    qzcam_isp_apply_settings();
    return true;

fail_mpi:
    RK_MPI_SYS_Exit();
    mpi_inited = false;
fail_isp:
    rk_aiq_uapi2_sysctl_stop(aiq_ctx, false);
    rk_aiq_uapi2_sysctl_deinit(aiq_ctx);
    aiq_ctx = NULL;
    return false;
}

const uint8_t *qzcam_isp_grab(int *width, int *height, int *stride, int timeout_ms)
{
    if (!aiq_ctx || !vi_inited) return NULL;
    if (frame_held) qzcam_isp_release();

    if (RK_MPI_VI_GetChnFrame(ISP_VI_PIPE, ISP_VI_CHN, &held_frame, timeout_ms) !=
        RK_SUCCESS)
        return NULL;
    frame_held = true;

    void *vir = RK_MPI_MB_Handle2VirAddr(held_frame.stVFrame.pMbBlk);
    if (!vir) {
        qzcam_isp_release();
        return NULL;
    }
    if (width) *width = (int)held_frame.stVFrame.u32Width;
    if (height) *height = (int)held_frame.stVFrame.u32Height;
    if (stride) *stride = (int)held_frame.stVFrame.u32VirWidth;
    return (const uint8_t *)vir;
}

void qzcam_isp_release(void)
{
    if (!frame_held) return;
    RK_MPI_VI_ReleaseChnFrame(ISP_VI_PIPE, ISP_VI_CHN, &held_frame);
    frame_held = false;
}

int qzcam_isp_virtual_height(void) { return frame_held ? (int)held_frame.stVFrame.u32VirHeight : 0; }
void *qzcam_isp_held_frame(void) { return frame_held ? &held_frame : NULL; }

void qzcam_isp_apply_settings(void)
{
    if (!aiq_ctx) return;
    qzcam_settings_t s = *qzcam_settings();
    bool first = !settings_applied;
    if (first || s.wb_mode != applied_settings.wb_mode || s.wb_kelvin != applied_settings.wb_kelvin) {
        rk_aiq_uapi2_setWBMode(aiq_ctx, s.wb_mode ? OP_MANUAL : OP_AUTO);
        static const unsigned temperature[] = {5500, 5500, 6500, 3200};
        if (s.wb_mode) rk_aiq_uapi2_setMWBCT(aiq_ctx, s.wb_mode < 4 ? temperature[s.wb_mode] : (unsigned)s.wb_kelvin);
    }
    if (first || s.sharpness != applied_settings.sharpness)
        rk_aiq_uapi2_setSharpness(aiq_ctx, (unsigned)s.sharpness);
    if (first || s.denoise != applied_settings.denoise || s.isp_denoise != applied_settings.isp_denoise) {
        unsigned strength = s.isp_denoise ? (unsigned)(s.isp_denoise * 25) : (unsigned)s.denoise;
        rk_aiq_uapi2_setMSpaNRStrth(aiq_ctx, strength > 0, strength);
        rk_aiq_uapi2_setMTNRStrth(aiq_ctx, strength > 0, strength / 2);
    }
    if (first || s.wdr != applied_settings.wdr || s.night_mode != applied_settings.night_mode) {
        rk_aiq_uapi2_setDrcGain(aiq_ctx, s.wdr ? 2.0f : 1.0f, 0.2f, 16.0f);
        rk_aiq_uapi2_setDrcLocalTMO(aiq_ctx, s.wdr ? 0.5f : 0.2f, 0.1f, s.night_mode ? 0.8f : 0.2f);
    }
    applied_settings = s; settings_applied = true;
}

void qzcam_isp_close(void)
{
    if (vi_inited) {
        RK_MPI_VI_DisableChn(ISP_VI_PIPE, ISP_VI_CHN);
        RK_MPI_VI_DisableDev(ISP_VI_DEV);
        vi_inited = false;
    }
    if (mpi_inited) {
        RK_MPI_SYS_Exit();
        mpi_inited = false;
    }
    if (aiq_ctx) {
        rk_aiq_uapi2_sysctl_stop(aiq_ctx, false);
        rk_aiq_uapi2_sysctl_deinit(aiq_ctx);
        aiq_ctx = NULL;
    }
    frame_held = false;
}

const char *qzcam_isp_last_error(void)
{
    return last_error[0] ? last_error : "ISP 取景不可用";
}

bool qzcam_isp_ready(void)
{
    return aiq_ctx != NULL;
}

/* EV 补偿：缩放 AE 的曝光时间路线上限（官方支持的 SetAeRangeEn 机制），
 * AE 仍保持自动 —— EV+ 时允许更长曝光，EV- 时压短，画面随之变亮/变暗。 */
void qzcam_isp_apply_ev(int ev_x10)
{
    if (!aiq_ctx) return;
    static float base_max_exp = -1.0f;   /* 第一次调用时记下 IQ 的默认路线 */

    Uapi_ExpSwAttrV2_t attr;
    if (rk_aiq_user_api2_ae_getExpSwAttr(aiq_ctx, &attr) != XCAM_RETURN_NO_ERROR)
        return;

    if (base_max_exp < 0)
        base_max_exp = attr.stAdvanced.SetLinAeRange.stExpTimeRange.Max;

    float factor = powf(2.0f, ev_x10 / 20.0f);       /* ±3EV → ×0.125..×8 */
    if (factor > 8.0f) factor = 8.0f;
    if (factor < 0.125f) factor = 0.125f;

    attr.stAdvanced.enable = true;
    attr.stAdvanced.SetAeRangeEn = true;
    attr.stAdvanced.SetLinAeRange.stExpTimeRange.Max = base_max_exp * factor;
    if (attr.stAdvanced.SetLinAeRange.stExpTimeRange.Max < 0.0001f)
        attr.stAdvanced.SetLinAeRange.stExpTimeRange.Max = 0.0001f;

    rk_aiq_user_api2_ae_setExpSwAttr(aiq_ctx, attr);
}

#endif /* !QZCAM_HAVE_MEDIA */
