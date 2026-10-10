#define _GNU_SOURCE

#include "cam_core.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cam_isp.h"
#include "cam_image.h"
#include "cam_record.h"
#include "cam_settings.h"
#include "qzcam.h"

#define PREVIEW_BUFFERS 3
#define CAPTURE_BUFFERS 4

typedef enum { SRC_NONE = 0, SRC_FAKE, SRC_V4L2, SRC_ISP } src_t;

typedef struct {
    void *start;
    size_t length;
} capture_buffer_t;

/* --------------------------------------------------------------- 全局状态 */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool worker_started;
static volatile bool stop_requested;
static bool running;
static src_t source = SRC_NONE;

/* 采集侧（V4L2） */
static int vfd = -1;
static bool use_mplane;
static uint32_t pixel_format;
static int cap_w, cap_h, cap_stride;
static int cap_uv_h;
static capture_buffer_t cap_buffers[CAPTURE_BUFFERS];
static unsigned cap_count;

/* 预览侧：三缓冲，面板尺寸 RGB565 */
static int panel_w, panel_h;
static uint8_t *frames[PREVIEW_BUFFERS];
static uint8_t *scratch;      /* 拍照时的复制目标（不与取景争缓冲） */
static int latest_index = -1; /* 最新完成的帧 */
static int shown_index = -1;  /* 界面正在渲染的那块，工作线程避让 */
static uint32_t frame_seq;
static int photo_count;
static int capture_want_w = 640, capture_want_h = 480;

static char backend_name[112] = "未启动";
static char status_text[176] = "取景未启动";

/* 真机不可用时的"现场信息"：直接显示在屏幕上。相机这类问题全在板子上，
 * 没串口的时候把原因摆在屏上，比只给一行"取景失败"有用得多。 */
static char diag_text[1024] = "";
static bool fake_forced;       /* QZCAM_BACKEND=fake：只在自检时用 */
static bool real_streaming;    /* 真机流真的在出帧（不是"打开了"就算数） */
static unsigned long long last_frame_ms;

static double fps_value;
static unsigned long long fps_window_start;
static unsigned fps_window_frames;
static pthread_mutex_t snapshot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t snapshot_cond = PTHREAD_COND_INITIALIZER;
static bool snapshot_pending;
static uint8_t *snapshot_nv12;
static int snapshot_w, snapshot_h;

static void snapshot_capture(const uint8_t *data)
{
    pthread_mutex_lock(&lock);
    if (snapshot_pending) {
        int w = cap_w & ~1, h = cap_h & ~1;
        uint8_t *copy = malloc((size_t)w * h * 3 / 2);
        if (copy && pixel_format == V4L2_PIX_FMT_NV12) {
            for (int y = 0; y < h; y++)
                memcpy(copy + (size_t)y * w, data + (size_t)y * cap_stride, w);
            const uint8_t *uv = data + (size_t)cap_stride * (cap_uv_h ? cap_uv_h : cap_h);
            for (int y = 0; y < h / 2; y++)
                memcpy(copy + (size_t)w * h + (size_t)y * w, uv + (size_t)y * cap_stride, w);
        } else if (copy) {
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x += 2) {
                    const uint8_t *p = data + (size_t)y * cap_stride + x * 2;
                    copy[y * w + x] = p[0]; copy[y * w + x + 1] = p[2];
                    if (!(y & 1)) {
                        int uv = w * h + y / 2 * w + x;
                        copy[uv] = p[1]; copy[uv + 1] = p[3];
                    }
                }
            }
        }
        snapshot_nv12 = copy; snapshot_w = w; snapshot_h = h;
        snapshot_pending = false;
        pthread_cond_signal(&snapshot_cond);
    }
    pthread_mutex_unlock(&lock);
}

/* --------------------------------------------------------------- 小工具 */

static void set_text(char *target, size_t size, const char *text)
{
    if (!target || size == 0) return;
    if (!text) text = "";
    strncpy(target, text, size - 1);
    target[size - 1] = '\0';
}

static void set_status(const char *text)
{
    set_text(status_text, sizeof(status_text), text);
}

static unsigned long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000);
}

static bool should_stop(void)
{
    bool stop;
    pthread_mutex_lock(&lock);
    stop = stop_requested;
    pthread_mutex_unlock(&lock);
    return stop;
}

/* ------------------------------------------------------------ 帧的发布 */

static int pick_buffer(void)
{
    int index = -1;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < PREVIEW_BUFFERS; i++) {
        if (i != latest_index && i != shown_index) {
            index = i;
            break;
        }
    }
    if (index < 0) index = (latest_index + 1) % PREVIEW_BUFFERS;
    pthread_mutex_unlock(&lock);
    return index;
}

static void publish(int index)
{
    unsigned long long now = monotonic_ms();
    pthread_mutex_lock(&lock);
    latest_index = index;
    frame_seq++;
    if (fps_window_start == 0) fps_window_start = now;
    fps_window_frames++;
    if (now - fps_window_start >= 1000) {
        fps_value = (double)fps_window_frames * 1000.0 / (double)(now - fps_window_start);
        fps_window_frames = 0;
        fps_window_start = now;
    }
    pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------------ V4L2 后端 */

/* rkisp 会把 mainpath/selfpath 等一堆节点都注册成 /dev/videoN，编号不固定，
 * 权威来源是 sysfs 里的名字表 —— 认 "rkisp_mainpath"。 */
static bool find_mainpath_node(char *path, size_t size)
{
    DIR *dir = opendir("/sys/class/video4linux");
    if (!dir) return false;

    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "video", 5) != 0) continue;
        char name_path[320];
        snprintf(name_path, sizeof(name_path), "/sys/class/video4linux/%s/name", entry->d_name);
        FILE *fp = fopen(name_path, "r");
        if (!fp) continue;
        char name[96] = "";
        if (fgets(name, sizeof(name), fp) && strstr(name, "rkisp_mainpath")) {
            snprintf(path, size, "/dev/%.31s", entry->d_name);
            found = true;
        }
        fclose(fp);
    }
    closedir(dir);
    return found;
}

static bool try_format(bool mplane, uint32_t want, int w, int h)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (mplane) {
        fmt.fmt.pix_mp.width = (uint32_t)w;
        fmt.fmt.pix_mp.height = (uint32_t)h;
        fmt.fmt.pix_mp.pixelformat = want;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) return false;
        if (fmt.fmt.pix_mp.pixelformat != want) return false;
        cap_w = (int)fmt.fmt.pix_mp.width;
        cap_h = (int)fmt.fmt.pix_mp.height;
        cap_stride = (int)fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    } else {
        fmt.fmt.pix.width = (uint32_t)w;
        fmt.fmt.pix.height = (uint32_t)h;
        fmt.fmt.pix.pixelformat = want;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(vfd, VIDIOC_S_FMT, &fmt) < 0) return false;
        if (fmt.fmt.pix.pixelformat != want) return false;
        cap_w = (int)fmt.fmt.pix.width;
        cap_h = (int)fmt.fmt.pix.height;
        cap_stride = (int)fmt.fmt.pix.bytesperline;
    }
    if (cap_stride <= 0) cap_stride = cap_w;   /* NV12 的 stride 就是宽度 */
    return cap_w > 0 && cap_h > 0;
}

static bool queue_buffer(unsigned index)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = use_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    if (use_mplane) {
        buf.m.planes = planes;
        buf.length = 1;
    }
    return ioctl(vfd, VIDIOC_QBUF, &buf) == 0;
}

static bool start_streaming(void)
{
    struct v4l2_requestbuffers request;
    memset(&request, 0, sizeof(request));
    request.count = CAPTURE_BUFFERS;
    request.type = use_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(vfd, VIDIOC_REQBUFS, &request) < 0 || request.count == 0) return false;
    cap_count = request.count > CAPTURE_BUFFERS ? CAPTURE_BUFFERS : request.count;

    for (unsigned i = 0; i < cap_count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = request.type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (use_mplane) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        if (ioctl(vfd, VIDIOC_QUERYBUF, &buf) < 0) return false;

        size_t length = use_mplane ? planes[0].length : buf.length;
        off_t offset = use_mplane ? (off_t)planes[0].m.mem_offset : (off_t)buf.m.offset;
        void *mapped = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, offset);
        if (mapped == MAP_FAILED) return false;
        cap_buffers[i].start = mapped;
        cap_buffers[i].length = length;
    }

    for (unsigned i = 0; i < cap_count; i++) {
        if (!queue_buffer(i)) return false;
    }

    enum v4l2_buf_type type = request.type;
    return ioctl(vfd, VIDIOC_STREAMON, &type) == 0;
}

/* /sys/class/video4linux/videoN/name 汇总成一行。真机打不开时这张表最有用：
 * 一眼能看出 rkisp_mainpath 到底有没有注册、有没有别的节点占着号。 */
static void scan_nodes(char *out, size_t size)
{
    out[0] = '\0';
    DIR *dir = opendir("/sys/class/video4linux");
    if (!dir) {
        snprintf(out, size, "(读不到 /sys/class/video4linux：内核没建这个目录)");
        return;
    }
    size_t used = 0;
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < 12) {
        if (strncmp(entry->d_name, "video", 5) != 0) continue;
        char path[320];
        snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char name[112] = "";
        if (fgets(name, sizeof(name), fp)) {
            size_t n = strlen(name);
            while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r')) name[--n] = '\0';
        }
        fclose(fp);
        if (used + 1 < size) {
            int written = snprintf(out + used, size - used, "%s%s=%s",
                                   used ? " " : "", entry->d_name, name[0] ? name : "?");
            if (written > 0) used += (size_t)written;
        }
        count++;
    }
    closedir(dir);
    if (used == 0) snprintf(out, size, "(一个 /dev/videoN 都没有)");
}

/* 该节点支持哪些像素格式 —— 用来判断"这是不是取景那个节点"。 */
static void list_formats(int fd, char *out, size_t size)
{
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < 8; i++) {
        struct v4l2_fmtdesc desc;
        memset(&desc, 0, sizeof(desc));
        desc.index = (uint32_t)i;
        desc.type = use_mplane ? (uint32_t)V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                               : (uint32_t)V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) break;
        if (used + 1 < size) {
            int written = snprintf(out + used, size - used, "%s%c%c%c%c",
                                   used ? " " : "",
                                   desc.pixelformat & 0xFF,
                                   (desc.pixelformat >> 8) & 0xFF,
                                   (desc.pixelformat >> 16) & 0xFF,
                                   (desc.pixelformat >> 24) & 0xFF);
            if (written > 0) used += (size_t)written;
        }
    }
    if (used == 0) snprintf(out, size, "(枚举不到格式)");
}

static bool open_v4l2(void)
{
    char node[64] = "";
    const char *forced = getenv("QZCAM_DEV");
    if (forced && forced[0]) snprintf(node, sizeof(node), "%s", forced);
    else if (!find_mainpath_node(node, sizeof(node))) {
        char nodes[256];
        scan_nodes(nodes, sizeof(nodes));
        snprintf(diag_text, sizeof(diag_text),
                 "没找到 rkisp_mainpath 取景节点。\n现有视频节点：%s\n"
                 "→ 相机模块没加载（cat /proc/modules | grep -E 'rkisp|sc3336'），"
                 "或传感器/CSI 没出流。", nodes);
        set_status("未发现 rkisp_mainpath 节点");
        return false;
    }

    vfd = open(node, O_RDWR | O_NONBLOCK);
    if (vfd < 0) {
        snprintf(diag_text, sizeof(diag_text),
                 "打开 %s 失败：%s (errno=%d)\n"
                 "→ 提示 busy 就是被别的程序占着（rkipc / 上一个 qzcam 没退干净）。",
                 node, strerror(errno), errno);
        set_status("打不开取景节点");
        return false;
    }

    /* 采集尺寸：ISP 支持缩放，所以优先要小的（省带宽、省 ISP 负载），要不到再往上试。
     * 官方文档里 rkisp_mainpath 的原生尺寸是 2304x1296(NV12)，所以那档放最后兜底。
     * QZCAM_CAPTURE=WxH 可以钉死一个尺寸，不再自动试。 */
    int sizes[3][2];
    int size_count = 0;
    const char *capture = getenv("QZCAM_CAPTURE");
    int w = 0, h = 0;
    if (capture && sscanf(capture, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        sizes[size_count][0] = w;
        sizes[size_count][1] = h;
        size_count++;
    } else {
        sizes[size_count][0] = 2304; sizes[size_count][1] = 1296; size_count++;
        sizes[size_count][0] = 1280; sizes[size_count][1] = 720;  size_count++;
        sizes[size_count][0] = 640;  sizes[size_count][1] = 480;  size_count++;
    }

    /* mainpath 一般只提供多平面接口，但两种都试；格式 NV12 优先、YUYV 兜底。 */
    const bool mplane_order[2] = { true, false };
    bool ok = false;
    for (int s = 0; s < size_count && !ok; s++) {
        for (int m = 0; m < 2 && !ok; m++) {
            for (int f = 0; f < 2 && !ok; f++) {
                uint32_t want = (f == 0) ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_YUYV;
                if (try_format(mplane_order[m], want, sizes[s][0], sizes[s][1])) {
                    use_mplane = mplane_order[m];
                    pixel_format = want;
                    ok = true;
                }
            }
        }
    }
    if (!ok) {
        char formats[160];
        list_formats(vfd, formats, sizeof(formats));
        close(vfd);
        vfd = -1;
        snprintf(diag_text, sizeof(diag_text),
                 "%s 不支持 NV12/YUYV（试过 640x480、1280x720、2304x1296）。\n"
                 "它支持的格式：%s\n"
                 "→ 可能不是 ISP 主通道。用 QZCAM_DEV=/dev/videoN 指个别的节点试试。",
                 node, formats);
        set_status("取景节点不支持 NV12/YUYV");
        return false;
    }
    if (!start_streaming()) {
        int err = errno;
        close(vfd);
        vfd = -1;
        snprintf(diag_text, sizeof(diag_text),
                 "VIDIOC_STREAMON 失败：%s (errno=%d，请求 %dx%d)\n"
                 "→ 节点在但出不了流：传感器/CSI 没起来，或该尺寸不支持"
                 "（试 QZCAM_CAPTURE=2304x1296）。", strerror(err), err, cap_w, cap_h);
        set_status("启动视频流失败");
        return false;
    }

    snprintf(backend_name, sizeof(backend_name), "V4L2 %s %dx%d %s", node, cap_w, cap_h,
             pixel_format == V4L2_PIX_FMT_NV12 ? "NV12" : "YUYV");
    diag_text[0] = '\0';
    set_status("取景中");
    return true;
}

/* --------------------------------------------------- ISP(RKMPI) 取景后端 */

/* 真机首选：rkaiq 3A + RKMPI VI 出 NV12（照 Echo-Mate / SDK 官方示例那条路）。
 * 裸 V4L2 直取绕过了 3A，图会偏暗/偏绿，甚至一帧不出 —— 所以它只是兜底。 */
static bool open_isp(void)
{
    static const int sizes[3][2] = {{2304,1296}, {1920,1080}, {1280,720}};
    int resolution = qzcam_settings()->resolution;
    if (resolution < 0 || resolution > 2) resolution = 0;
    int w = sizes[resolution][0], h = sizes[resolution][1];
    const char *capture = getenv("QZCAM_CAPTURE");
    int cw = 0, chh = 0;
    if (capture && sscanf(capture, "%dx%d", &cw, &chh) == 2 && cw > 0 && chh > 0) {
        w = cw;
        h = chh;
    }
    if (!qzcam_isp_open(w, h)) return false;

    /* 交给共用的转码函数：按 NV12 处理，stride 用驱动给的行宽 */
    pixel_format = V4L2_PIX_FMT_NV12;
    cap_w = w;
    cap_h = h;
    cap_stride = w;
    snprintf(backend_name, sizeof(backend_name), "ISP(RKMPI) %dx%d NV12", w, h);
    diag_text[0] = '\0';
    set_status("取景中（ISP）");
    return true;
}

/* 依次试：ISP（首选）→ V4L2 直取（兜底）。成功时 source / backend_name / cap_*
 * 都已就位；都失败时把两条路的原因和现场写进 diag_text（ISP 的原因排最前，
 * 那才是根因）。 */
static bool open_any(void)
{
    cap_uv_h = 0;
    if (open_isp()) {
        source = SRC_ISP;
        return true;
    }
    char isp_err[224];
    snprintf(isp_err, sizeof(isp_err), "%s", qzcam_isp_last_error());

    if (open_v4l2()) {
        source = SRC_V4L2;
        set_status("取景中（V4L2 兜底：未过 ISP 3A，可能偏色）");
        return true;
    }
    char v4l2_err[176];
    snprintf(v4l2_err, sizeof(v4l2_err), "%s", status_text);

    char nodes[192];
    scan_nodes(nodes, sizeof(nodes));
    snprintf(diag_text, sizeof(diag_text),
             "ISP 取景失败：%s\nV4L2 直取失败：%s\n现有视频节点：%s\n"
             "→ 依次查：① 模块 cat /proc/modules | grep -E 'rkisp|sc3336|rockit'  "
             "② IQ 文件 ls /etc/iqfiles  "
             "③ dmesg | grep -iE 'sc3336|rkisp|rockit'",
             isp_err, v4l2_err, nodes);
    set_text(backend_name, sizeof(backend_name), "相机未就绪");
    set_status("相机未就绪，正在重试…");
    return false;
}

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

/* NV12/YUYV → 面板尺寸 RGB565（最近邻缩放）。ISP 出的帧比面板大得多，缩放这
 * 一步是必须的；不做插值是为了省掉一帧 7 万多次的浮点乘除。 */
/* 列采样映射表：sx 只跟 x 有关，逐像素做除法太浪费（每帧 7 万次 32 位除法）。
 * 捕获/面板尺寸变了才重建。 */
static int *sx_lut;
static int sx_lut_panel_w = -1;
static int sx_lut_cap_w = -1;

static void rebuild_sx_lut(void)
{
    if (!sx_lut || sx_lut_panel_w != panel_w) {
        free(sx_lut);
        sx_lut = malloc(sizeof(int) * (size_t)panel_w);
        sx_lut_panel_w = panel_w;
    }
    for (int x = 0; x < panel_w; x++)
        sx_lut[x] = x * cap_w / panel_w;
    sx_lut_cap_w = cap_w;
}

static void convert_frame(const uint8_t *data)
{
    snapshot_capture(data);
    int index = pick_buffer();
    uint8_t *dst = frames[index];

    if (sx_lut_cap_w != cap_w || sx_lut_panel_w != panel_w) rebuild_sx_lut();

    if (pixel_format == V4L2_PIX_FMT_NV12) {
        const uint8_t *y_plane = data;
        const uint8_t *uv_plane = data + (size_t)cap_stride * (size_t)(cap_uv_h ? cap_uv_h : cap_h);
        for (int y = 0; y < panel_h; y++) {
            int sy = y * cap_h / panel_h;
            const uint8_t *yrow = y_plane + (size_t)sy * (size_t)cap_stride;
            const uint8_t *uvrow = uv_plane + (size_t)(sy / 2) * (size_t)cap_stride;
            uint16_t *orow = (uint16_t *)(dst + (size_t)y * (size_t)panel_w * 2);
            for (int x = 0; x < panel_w; x++) {
                int sx = sx_lut[x];
                orow[x] = yuv_to_rgb565(yrow[sx], uvrow[sx & ~1], uvrow[(sx & ~1) + 1]);
            }
        }
    } else {
        for (int y = 0; y < panel_h; y++) {
            int sy = y * cap_h / panel_h;
            const uint8_t *row = data + (size_t)sy * (size_t)cap_stride;
            uint16_t *orow = (uint16_t *)(dst + (size_t)y * (size_t)panel_w * 2);
            for (int x = 0; x < panel_w; x++) {
                int sx = sx_lut[x] & ~1;
                const uint8_t *p = row + (size_t)sx * 2;
                orow[x] = yuv_to_rgb565(p[0], p[1], p[3]);
            }
        }
    }

    qzcam_settings_apply((uint16_t *)dst, panel_w, panel_h);
    qzcam_rec_submit(dst, panel_w, panel_h);   /* 录像在录的话，这一帧落盘 */
    publish(index);
}

static bool capture_one(void)
{
    struct pollfd pfd = { .fd = vfd, .events = POLLIN };
    if (poll(&pfd, 1, 200) <= 0) return false;

    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = use_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (use_mplane) {
        buf.m.planes = planes;
        buf.length = 1;
    }
    if (ioctl(vfd, VIDIOC_DQBUF, &buf) < 0) return false;

    convert_frame((const uint8_t *)cap_buffers[buf.index].start);
    queue_buffer(buf.index);
    return true;
}

/* ------------------------------------------------------------ 测试后端 */

/* 彩条 + 一个走动的白块：没有任何摄像头时也能看出「画面在动、链路是活的」。 */
static void render_fake_frame(void)
{
    static unsigned tick;
    static const uint16_t bars[8] = {
        0xF800, 0xFC00, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF,
    };
    tick++;
    pthread_mutex_lock(&lock);
    if (snapshot_pending) {
        static const int sizes[3][2] = {{2304,1296}, {1920,1080}, {1280,720}};
        int res = qzcam_settings()->resolution;
        if (res < 0 || res > 2) res = 0;
        int w = sizes[res][0], h = sizes[res][1];
        snapshot_nv12 = malloc((size_t)w * h * 3 / 2);
        snapshot_w = w; snapshot_h = h;
        if (snapshot_nv12) {
            memset(snapshot_nv12 + w * h, 128, (size_t)w * h / 2);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    snapshot_nv12[y * w + x] = (uint8_t)(40 + x * 140 / w + (((x / 16) ^ (y / 16)) & 1) * 40);
        }
        snapshot_pending = false;
        pthread_cond_signal(&snapshot_cond);
    }
    pthread_mutex_unlock(&lock);

    int index = pick_buffer();
    uint16_t *dst = (uint16_t *)frames[index];
    unsigned advance = tick * 3;

    for (int y = 0; y < panel_h; y++) {
        uint16_t *row = dst + (size_t)y * (size_t)panel_w;
        int bands = (panel_w + 39) / 40;
        for (int x = 0; x < panel_w; x++) {
            int band = (int)(((unsigned)x + advance) * (unsigned)bands / (unsigned)panel_w);
            row[x] = bars[(band + (y / 24)) & 7];
        }
    }

    int cx = panel_w > 40 ? (int)(tick * 4) % (panel_w - 32) : 0;
    for (int y = panel_h / 2 - 12; y < panel_h / 2 + 12; y++) {
        if (y < 0 || y >= panel_h) continue;
        for (int x = cx; x < cx + 24 && x < panel_w; x++) {
            dst[(size_t)y * (size_t)panel_w + x] = 0xFFFF;
        }
    }

    qzcam_settings_apply(dst, panel_w, panel_h);
    qzcam_rec_submit((const uint8_t *)dst, panel_w, panel_h);
    publish(index);
}

/* --------------------------------------------------------------- 工作线程 */

/* 关流 + 释放 buffer + 关节点。停止和"断流重连"都走这一份。 */
static void close_v4l2(void)
{
    if (vfd < 0) return;
    enum v4l2_buf_type type = use_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                         : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(vfd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < cap_count; i++) {
        if (cap_buffers[i].start) munmap(cap_buffers[i].start, cap_buffers[i].length);
        cap_buffers[i].start = NULL;
    }
    cap_count = 0;
    close(vfd);
    vfd = -1;
}

static void *worker_main(void *argument)
{
    (void)argument;
    unsigned long long next_retry = 0;

    while (!should_stop()) {
        if (source == SRC_ISP) {
            int w = 0, h = 0, stride = 0;
            const uint8_t *nv12 = qzcam_isp_grab(&w, &h, &stride, 500);
            if (nv12) {
                if (w > 0 && h > 0 && stride > 0) {
                    cap_w = w;
                    cap_h = h;
                    cap_stride = stride;
                    cap_uv_h = qzcam_isp_virtual_height();
                    convert_frame(nv12);
                }
                qzcam_isp_release();
                last_frame_ms = monotonic_ms();
                real_streaming = true;
                continue;
            }
            usleep(5 * 1000);
            if (monotonic_ms() - last_frame_ms > 3000) {
                real_streaming = false;
                set_status("取景中断，正在重连…");
                qzcam_isp_close();
                source = SRC_NONE;
                next_retry = 0;
            }
            continue;
        }

        if (source == SRC_V4L2) {
            if (capture_one()) {
                last_frame_ms = monotonic_ms();
                real_streaming = true;
                continue;
            }
            /* 连续 3 秒拿不到帧就当流断了：关掉重开。开机时传感器/CSI 偶尔比
             * 程序慢、或前一次 open 拿到的是"空流"，这样能自愈，不用重启程序。 */
            usleep(5 * 1000);
            if (monotonic_ms() - last_frame_ms > 3000) {
                real_streaming = false;
                set_status("取景中断，正在重连…");
                close_v4l2();
                source = SRC_NONE;
                next_retry = 0;
            }
            continue;
        }

        if (fake_forced) {
            const qzcam_settings_t *s = qzcam_settings();
            int fps = s->fps > 0 ? s->fps : 15;
            if (fps > 30) fps = 30;
            render_fake_frame();
            usleep((useconds_t)(1000000 / fps));
            continue;
        }

        /* 真机没就绪：每 2 秒重连一次，界面这期间显示 diag_text 里的原因。
         * 这里**故意不退回彩条** —— 假画面会让人以为相机在工作。 */
        unsigned long long now = monotonic_ms();
        if (now >= next_retry) {
            next_retry = now + 2000;
            if (open_any()) {
                last_frame_ms = monotonic_ms();
                real_streaming = true;
            }
        } else {
            usleep(100 * 1000);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- 对外 */

bool qzcam_cam_start(void)
{
    pthread_mutex_lock(&lock);
    if (running || worker_started) {
        pthread_mutex_unlock(&lock);
        return false;
    }

    panel_w = qzcam_panel_w;
    panel_h = qzcam_panel_h;
    if (panel_w < 16 || panel_h < 16) {
        set_status("面板尺寸异常");
        pthread_mutex_unlock(&lock);
        return false;
    }

    size_t bytes = (size_t)panel_w * (size_t)panel_h * 2;
    for (int i = 0; i < PREVIEW_BUFFERS; i++) {
        if (!frames[i]) frames[i] = malloc(bytes);
        if (!frames[i]) {
            set_status("内存不足，无法分配取景缓冲");
            pthread_mutex_unlock(&lock);
            return false;
        }
    }
    if (!scratch) scratch = malloc(bytes);
    latest_index = -1;
    shown_index = -1;
    frame_seq = 0;

    const char *forced = getenv("QZCAM_BACKEND");
    fake_forced = forced && strcmp(forced, "fake") == 0;
    const bool force_v4l2 = forced && strcmp(forced, "v4l2") == 0;

    real_streaming = false;
    diag_text[0] = '\0';
    source = SRC_NONE;

    if (fake_forced) {
        /* 只有显式要求才给彩条：开发机/CI 上把整条链路跑通用的 */
        source = SRC_FAKE;
        set_text(backend_name, sizeof(backend_name), "测试后端（彩条）");
        set_status("取景中（测试画面）");
    } else if (open_any()) {
        last_frame_ms = monotonic_ms();
        real_streaming = true;
    } else if (force_v4l2) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    /* 两条真机路都不通时不退彩条：交给工作线程每 2 秒重连，界面显示 diag_text
     * —— 你看到的是"为什么没画面"，不是一张假画面。 */

    stop_requested = false;
    if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
        set_status("创建取景线程失败");
        source = SRC_NONE;
        pthread_mutex_unlock(&lock);
        return false;
    }
    worker_started = true;
    running = true;
    pthread_mutex_unlock(&lock);
    return true;
}

void qzcam_cam_stop(void)
{
    pthread_mutex_lock(&lock);
    if (!worker_started) {
        pthread_mutex_unlock(&lock);
        return;
    }
    stop_requested = true;
    pthread_mutex_unlock(&lock);

    pthread_join(worker, NULL);

    pthread_mutex_lock(&lock);
    worker_started = false;
    running = false;
    close_v4l2();
    qzcam_isp_close();
    source = SRC_NONE;
    real_streaming = false;
    set_text(backend_name, sizeof(backend_name), "未启动");
    set_status("取景未启动");
    pthread_mutex_unlock(&lock);
}

bool qzcam_cam_running(void)
{
    pthread_mutex_lock(&lock);
    bool value = running;
    pthread_mutex_unlock(&lock);
    return value;
}

/* 真机画面可用吗：真机流在出帧（fake 后端按"在跑"算，那是自检）。
 * 界面用它决定要不要盖"相机无信号 + 原因"的浮层。 */
bool qzcam_cam_healthy(void)
{
    pthread_mutex_lock(&lock);
    bool value = fake_forced ? running : real_streaming;
    pthread_mutex_unlock(&lock);
    return value;
}

/* 真机起不来时的现场信息（多行）。可用时为空串。 */
const char *qzcam_cam_diag(void)
{
    return diag_text;
}

const char *qzcam_cam_backend(void)
{
    return backend_name;
}

const char *qzcam_cam_status(void)
{
    return status_text;
}

int qzcam_cam_frame_w(void)
{
    return panel_w;
}

int qzcam_cam_frame_h(void)
{
    return panel_h;
}

const uint8_t *qzcam_cam_frame(uint32_t *id)
{
    const uint8_t *result = NULL;
    pthread_mutex_lock(&lock);
    if (latest_index >= 0) {
        shown_index = latest_index;
        if (id) *id = frame_seq;
        result = frames[latest_index];
    } else if (id) {
        *id = 0;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

double qzcam_cam_fps(void)
{
    pthread_mutex_lock(&lock);
    double value = fps_value;
    pthread_mutex_unlock(&lock);
    return value;
}

int qzcam_cam_photos(void)
{
    pthread_mutex_lock(&lock);
    int value = photo_count;
    pthread_mutex_unlock(&lock);
    return value;
}

/* ------------------------------------------------------------ 快门存图 */

static void put_u32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value & 0xFF);
    target[1] = (uint8_t)((value >> 8) & 0xFF);
    target[2] = (uint8_t)((value >> 16) & 0xFF);
    target[3] = (uint8_t)((value >> 24) & 0xFF);
}

static void put_u16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value & 0xFF);
    target[1] = (uint8_t)((value >> 8) & 0xFF);
}

/* 24 位 BMP：不需要任何图像库（板上没装 libjpeg/ffmpeg），文件任何看图工具都能开。
 * 硬编码 JPEG 的接入点是这里 —— 换成 VENC 编码即可，界面与调用方都不用动。 */
static bool write_bmp(const char *path, const uint8_t *rgb565, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    int row = (w * 3 + 3) & ~3;
    uint8_t header[54];
    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put_u32(header + 2, (uint32_t)(54 + row * h));
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, (uint32_t)w);
    put_u32(header + 22, (uint32_t)h);
    put_u16(header + 26, 1);
    put_u16(header + 28, 24);
    put_u32(header + 34, (uint32_t)(row * h));

    bool ok = fwrite(header, 1, sizeof(header), fp) == sizeof(header);
    uint8_t *line = ok ? malloc((size_t)row) : NULL;
    if (!line) ok = false;

    for (int y = h - 1; ok && y >= 0; y--) {
        const uint16_t *src = (const uint16_t *)(rgb565 + (size_t)y * (size_t)w * 2);
        for (int x = 0; x < w; x++) {
            uint16_t pixel = src[x];
            line[x * 3 + 0] = (uint8_t)((pixel & 0x1F) << 3);
            line[x * 3 + 1] = (uint8_t)(((pixel >> 5) & 0x3F) << 2);
            line[x * 3 + 2] = (uint8_t)(((pixel >> 11) & 0x1F) << 3);
        }
        for (int x = w * 3; x < row; x++) line[x] = 0;
        if (fwrite(line, 1, (size_t)row, fp) != (size_t)row) ok = false;
    }

    free(line);
    fclose(fp);
    return ok;
}

bool qzcam_cam_snapshot(char *path, unsigned path_size)
{
    pthread_mutex_lock(&snapshot_lock);
    pthread_mutex_lock(&lock);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    snapshot_pending = true;
    while (snapshot_pending && running) {
        if (pthread_cond_timedwait(&snapshot_cond, &lock, &deadline) == ETIMEDOUT) break;
    }
    snapshot_pending = false;
    uint8_t *nv12 = snapshot_nv12;
    snapshot_nv12 = NULL;
    int w = snapshot_w, h = snapshot_h;
    pthread_mutex_unlock(&lock);
    if (!nv12) {
        set_status("无法获取高清帧，请检查相机信号");
        pthread_mutex_unlock(&snapshot_lock);
        return false;
    }
    qzcam_settings_t s = *qzcam_settings();
    time_t now = time(NULL);
    struct tm parts;
    localtime_r(&now, &parts);
    char name[96], file[320], tmp[328], original[328];
    static unsigned serial;
    snprintf(name, sizeof(name), "IMG_%04d%02d%02d_%02d%02d%02d_%06u.jpg",
             parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
             parts.tm_hour, parts.tm_min, parts.tm_sec, ++serial);
    snprintf(file, sizeof(file), "%s/%s", qzcam_settings_photo_dir(), name);
    snprintf(tmp, sizeof(tmp), "%s.part", file);
    if (s.keep_original && s.enhancement) {
        uint8_t *raw = qzcam_image_rgb(nv12, w, h, &s);
        snprintf(original, sizeof(original), "%s.original.jpg", file);
        if (raw) qzcam_image_jpeg(original, raw, w, h, s.jpeg_quality);
        free(raw);
    }
    bool ok = qzcam_image_enhance(nv12, w, h, &s);
    uint8_t *rgb = ok ? qzcam_image_rgb(nv12, w, h, &s) : NULL;
    free(nv12);
    ok = rgb && qzcam_image_jpeg(tmp, rgb, w, h, s.jpeg_quality) && rename(tmp, file) == 0;
    if (ok) {
        char thumb[340];
        snprintf(thumb, sizeof(thumb), "%s.thumb.jpg", file);
        qzcam_image_thumb(thumb, rgb, w, h);
    } else {
        unlink(tmp);
        set_status("照片保存失败（内存不足、磁盘满或只读）");
    }
    free(rgb);
    pthread_mutex_lock(&lock);
    if (ok) photo_count++;
    pthread_mutex_unlock(&lock);
    pthread_mutex_unlock(&snapshot_lock);
    if (ok && path && path_size) snprintf(path, path_size, "%s", file);
    return ok;
}
