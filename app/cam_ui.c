#include "cam_ui.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cam_core.h"
#include "cam_player.h"
#include "cam_record.h"
#include "cam_settings.h"
#include "cam_storage.h"
#include "font.h"
#include "lvgl.h"
#include "qzcam.h"

/* 一台运动相机该有的界面（Apple 风格，见 skills/apple-design）：
 *   黑底 + #1C1C1E 卡片 + 运动橙 #FF6B35 强调，iOS 绿开关；
 *   顶部：录制胶囊（呼吸红点+计时）与规格胶囊（2304×1296·30fps·H.265）；
 *   底部悬浮控制条：相册 / 大快门（白环+内圆）/ 模式 / 设置；
 *   模式选择：视频·照片·慢动作·延时·循环 横向卡片；
 *   设置页：iOS 分组列表；参数调节：底部弹出面板（EV/白平衡/锐度/降噪）。
 * 320x240 上所有 pt 按 1pt≈1px 折算；动画遵循 skills/animate：
 * 入场 ease-out ≤250ms、按压即时反馈、可中断、出场走原路。 */

#define COLOR_SCR      lv_color_hex(0x000000)
#define COLOR_CARD     lv_color_hex(0x1C1C1E)
#define COLOR_CARD_2   lv_color_hex(0x2C2C2E)
#define COLOR_BAR      lv_color_hex(0x0B0B0F)
#define COLOR_TEXT     lv_color_hex(0xFFFFFF)
#define COLOR_DIM      lv_color_hex(0x999999)   /* 60% 白 */
#define COLOR_ACCENT   lv_color_hex(0xFF6B35)   /* 运动橙 */
#define COLOR_REC      lv_color_hex(0xFF3B30)
#define COLOR_OK       lv_color_hex(0x30D158)   /* iOS 绿（开关） */
#define COLOR_SHUTTER  lv_color_hex(0xFFFFFF)

typedef enum { MODE_PHOTO = 0, MODE_VIDEO } capture_mode_t;

/* 拍摄模式（settings.mode 的取值与文案），卡片选择器用 */
static const char *const mode_titles[5] = { "视频", "照片", "慢动作", "延时", "循环" };
static const char *const mode_icons[5]  = { "影", "照", "慢", "延", "环" };
static const char *const mode_specs[5]  = {
    "2304×1296 · 30fps",
    "3MP 单张",
    "1296×720 · 插帧",
    "0.5~10s 间隔",
    "1~10min 分段",
};

static lv_obj_t *preview_image;
static lv_image_dsc_t preview_dsc;
static uint32_t shown_seq;
static bool preview_ready;

static lv_obj_t *status_label;
static lv_obj_t *clock_label;
static lv_obj_t *storage_label;
static lv_obj_t *fps_label;
static lv_obj_t *photo_label;
static lv_obj_t *toast_label;
static lv_obj_t *flash;
static lv_obj_t *drawer;
static lv_obj_t *gallery;
static lv_obj_t *gallery_list;
static lv_obj_t *viewer;
static lv_obj_t *viewer_image;

/* 视频播放器（板载 AVI 回放） */
static lv_obj_t *player_screen;
static lv_obj_t *player_image;
static lv_obj_t *player_time;
static lv_obj_t *player_bar;
static lv_timer_t *player_timer;
static bool player_playing;
static lv_image_dsc_t player_dsc;
static int player_total;
static lv_obj_t *grid_lines[4];
static lv_obj_t *shutter_button;
static lv_obj_t *shutter_core;      /* 快门内圆：视频模式变红 */

/* 顶部录制胶囊：呼吸红点 + 等宽计时 */
static lv_obj_t *rec_capsule;
static lv_obj_t *rec_dot;
static lv_obj_t *rec_time_label;

/* 模式选择器（横向卡片） */
static lv_obj_t *mode_sheet;
static lv_obj_t *mode_cards[5];

/* 设置页（iOS 分组列表） */
static lv_obj_t *settings_page;

static lv_timer_t *toast_timer;

/* 自拍倒计时 / 连拍状态机 */
static lv_obj_t *countdown_label;
static lv_timer_t *capture_timer;
static int step_mode;    /* 0=空闲 1=自拍倒计时 2=连拍中 */
static int step_left;
static bool settings_dirty;

static void sheet_slide_in(lv_obj_t *sheet);
static void sheet_slide_out(lv_obj_t *sheet);

/* 滑出动画结束后把面板藏起来（动画回调拿到的是 lv_anim_t，取 var 才是对象） */
static void sheet_hide_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}         /* 改过参数 → 由 preview_timer 兜底落盘 */

/* 无信号浮层：真机不出画时盖在预览上，把"卡在哪一步"显示出来 */
static lv_obj_t *nosignal_panel;
static lv_obj_t *nosignal_status;
static lv_obj_t *nosignal_diag;
static bool shown_healthy = true;
static char shown_diag[1024];
static int shown_photos = -1;
static capture_mode_t capture_mode = MODE_PHOTO;

/* ------------------------------------------------------------------ 小工具 */

static lv_obj_t *make_bar(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, qzcam_panel_w, height);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_style_bg_color(bar, COLOR_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    return bar;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int px, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, qzcam_font(px), 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int w, int h, int radius)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_radius(button, radius, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1C1C22), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_t *label = make_label(button, text, 14, COLOR_TEXT);
    lv_obj_center(label);
    return button;
}

static void toast_hide(lv_timer_t *timer)
{
    lv_obj_add_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

static void toast(const char *text)
{
    lv_label_set_text(toast_label, text);
    lv_obj_remove_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast_label);
    if (!toast_timer) {
        toast_timer = lv_timer_create(toast_hide, 2000, NULL);
        lv_timer_pause(toast_timer);
    }
    lv_timer_reset(toast_timer);
    lv_timer_resume(toast_timer);
}

static void flash_off(lv_timer_t *timer)
{
    lv_obj_set_style_opa(flash, LV_OPA_TRANSP, 0);
    lv_timer_delete(timer);
}

static void apply_overlays(void)
{
    const qzcam_settings_t *s = qzcam_settings();
    for (int i = 0; i < 4; i++) {
        if (s->grid) lv_obj_remove_flag(grid_lines[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(grid_lines[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s->timestamp) lv_obj_remove_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN);
}

/* 存储余量显示：优先给人类可读的 G/M */
static void update_storage_label(void)
{
    static int tick;
    if (tick++ % 30 != 0) return;      /* 约每 1 秒刷一次；第一次立即刷 */

    unsigned long long free_bytes = qzcam_storage_free_bytes();
    int percent = qzcam_storage_free_percent();
    char size[24];
    if (free_bytes >= (1ULL << 30)) snprintf(size, sizeof(size), "%.1fG", (double)free_bytes / (double)(1ULL << 30));
    else snprintf(size, sizeof(size), "%lluM", free_bytes / (1024ULL * 1024ULL));
    lv_label_set_text_fmt(storage_label, "%s %s %d%%", qzcam_storage_label(), size, percent);
    if (percent < 10) lv_obj_set_style_text_color(storage_label, COLOR_REC, 0);
    else lv_obj_set_style_text_color(storage_label, COLOR_DIM, 0);
}

/* -------------------------------------------------------------- 取景刷新 */

static void gallery_refresh(void);
static void preview_timer(lv_timer_t *timer)
{
    (void)timer;
    uint32_t id = 0;
    const uint8_t *pixels = qzcam_cam_frame(&id);

    if (pixels && id != shown_seq && !player_timer) {
        /* 播放器打开时取景暂停：视频是全屏刷新，别跟它抢 */
        shown_seq = id;
        if (!preview_ready) {
            preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            preview_dsc.header.w = (uint32_t)qzcam_cam_frame_w();
            preview_dsc.header.h = (uint32_t)qzcam_cam_frame_h();
            preview_dsc.header.stride = (uint32_t)qzcam_cam_frame_w() * 2;
            preview_dsc.data_size = (uint32_t)(qzcam_cam_frame_w() * qzcam_cam_frame_h() * 2);
            preview_dsc.data = pixels;
            lv_image_set_src(preview_image, &preview_dsc);
            preview_ready = true;
        } else {
            preview_dsc.data = pixels;
            lv_obj_invalidate(preview_image);
        }
    }

    lv_label_set_text(status_label, qzcam_cam_status());

    /* 无信号浮层：真机没出画就显示（含正在重连的那段时间），出画了自动收掉。 */
    bool healthy = qzcam_cam_healthy();
    if (healthy != shown_healthy) {
        shown_healthy = healthy;
        if (healthy) lv_obj_add_flag(nosignal_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(nosignal_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (!healthy) {
        const char *diag = qzcam_cam_diag();
        if (!diag || !diag[0]) diag = "正在重试打开相机…";
        if (strcmp(diag, shown_diag) != 0) {
            snprintf(shown_diag, sizeof(shown_diag), "%s", diag);
            lv_label_set_text(nosignal_diag, shown_diag);
        }
        lv_label_set_text(nosignal_status, qzcam_cam_status());
    }

    static int tick;
    if (++tick >= 15) {
        tick = 0;
        lv_label_set_text_fmt(fps_label, "%.1f fps", qzcam_cam_fps());
        update_storage_label();
    }

    int photos = qzcam_cam_photos();
    if (photos != shown_photos) {
        shown_photos = photos;
        lv_label_set_text_fmt(photo_label, "照片 %d", photos);
    }

    /* 录制自动停止（写失败/空间不足）→ 提示原因并刷新相册 */
    static bool prev_rec_active;
    bool rec_now = qzcam_rec_active();
    if (prev_rec_active && !rec_now) {
        const char *reason = qzcam_rec_stop_reason();
        if (reason[0]) toast(reason);
        gallery_refresh();
    }
    prev_rec_active = rec_now;

    /* 录制状态胶囊：呼吸红点（1s 正弦明暗，状态指示）+ 等宽计时 */
    if (qzcam_rec_active()) {
        int seconds = qzcam_rec_seconds();
        lv_label_set_text_fmt(rec_time_label, "%02d:%02d", seconds / 60, seconds % 60);
        int opa = 90 + (int)(165.0 * (0.5 + 0.5 * sin(2.0 * 3.14159265 *
                       (double)(lv_tick_get() % 1000) / 1000.0)));
        lv_obj_set_style_opa(rec_dot, (lv_opa_t)opa, 0);
        lv_obj_remove_flag(rec_capsule, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(rec_capsule, LV_OBJ_FLAG_HIDDEN);
    }

    /* 改过的设置 2 秒节流落盘（拍半截断电也不怕：rename 原子替换） */
    if (settings_dirty) {
        static unsigned last_save;
        unsigned now = lv_tick_get();
        if (now - last_save >= 2000) {
            last_save = now;
            settings_dirty = false;
            qzcam_settings_save();
        }
    }
}

static void clock_timer(lv_timer_t *timer)
{
    (void)timer;
    time_t now = time(NULL);
    struct tm parts;
    localtime_r(&now, &parts);
    lv_label_set_text_fmt(clock_label, "%02d:%02d:%02d", parts.tm_hour, parts.tm_min, parts.tm_sec);
}

/* ------------------------------------------------------------------ 相册 */

typedef struct {
    char name[80];
    unsigned long long size;
    bool video;
} gallery_entry_t;

static void gallery_close_cb(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(gallery, LV_OBJ_FLAG_HIDDEN);
}

static void viewer_close_cb(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(viewer, LV_OBJ_FLAG_HIDDEN);
}

static void player_stop(void)
{
    player_playing = false;
    if (player_timer) {
        lv_timer_del(player_timer);
        player_timer = NULL;
    }
    qzcam_player_close();
    lv_obj_add_flag(player_screen, LV_OBJ_FLAG_HIDDEN);
}

static void player_close_cb(lv_event_t *event)
{
    (void)event;
    player_stop();
}

/* 渲染指定帧（seek 后也要能看到画面，暂停时同样有效） */
static void player_render_at(int frame)
{
    qzcam_player_seek_frame(frame);
    const uint8_t *f = qzcam_player_next_frame();
    if (!f) return;
    player_dsc.data = f;
    lv_image_set_src(player_image, &player_dsc);
    lv_obj_invalidate(player_image);

    int cur = qzcam_player_frame_index();
    int total = player_total;
    lv_bar_set_value(player_bar, total ? (int32_t)(cur * 100 / total) : 0, LV_ANIM_OFF);
    lv_label_set_text_fmt(player_time, "%02d:%02d / %02d:%02d",
                          cur / 60 / qzcam_player_fps(), cur / qzcam_player_fps() % 60,
                          total / 60 / qzcam_player_fps(), total / qzcam_player_fps() % 60);
}

/* 点击播放器画面：左 1/3 快退 5s，中 1/3 暂停/继续，右 1/3 快进 5s */
static void player_area_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(player_screen, &a);
    int rel = p.x - a.x1;
    int w = a.x2 - a.x1;
    int fps = qzcam_player_fps() > 0 ? qzcam_player_fps() : 15;

    if (rel < w / 3) {
        player_render_at(qzcam_player_frame_index() - 5 * fps);
    } else if (rel > w - w / 3) {
        player_render_at(qzcam_player_frame_index() + 5 * fps);
    } else {
        player_playing = !player_playing;
        if (player_playing && player_timer)
            lv_timer_resume(player_timer);
        else if (player_timer)
            lv_timer_pause(player_timer);
    }
}

/* 进度条点击定位：把点击 x 换算成帧号 */
static void player_seek_strip_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(player_screen, &a);
    int rel = p.x - a.x1;
    int w = a.x2 - a.x1;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    player_render_at((int)((long long)player_total * rel / (w ? w : 1)));
}

static void player_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!player_playing) return;

    const uint8_t *f = qzcam_player_next_frame();
    if (!f) {
        qzcam_player_seek_frame(0);      /* 循环播放；要停用“返回” */
        f = qzcam_player_next_frame();
        if (!f) {
            player_stop();
            return;
        }
    }
    player_dsc.data = f;
    lv_image_set_src(player_image, &player_dsc);   /* 每帧重设：换了 data 不重设，v9 不会重画 */
    lv_obj_invalidate(player_image);

    int cur = qzcam_player_frame_index();
    int total = player_total;
    lv_bar_set_value(player_bar, total ? (int32_t)(cur * 100 / total) : 0, LV_ANIM_OFF);
    lv_label_set_text_fmt(player_time, "%02d:%02d / %02d:%02d",
                          cur / 60 / qzcam_player_fps(), cur / qzcam_player_fps() % 60,
                          total / 60 / qzcam_player_fps(), total / qzcam_player_fps() % 60);
}

static void gallery_item_cb(lv_event_t *event)
{
    gallery_entry_t *entry = (gallery_entry_t *)lv_event_get_user_data(event);
    if (!entry) return;

    char path[256];
    snprintf(path, sizeof(path), "A:%s/%s", qzcam_storage_dir(), entry->name);

    if (entry->video) {
        /* 板载回放：AVI 是自家写的未压缩 RGB565，直接读块+翻行 */
        char fs_path[512];
        snprintf(fs_path, sizeof(fs_path), "%s/%s", qzcam_storage_dir(), entry->name);
        if (!qzcam_player_open(fs_path)) {
            toast("打不开录像文件");
            return;
        }
        int w = qzcam_player_frame_w(), h = qzcam_player_frame_h();
        memset(&player_dsc, 0, sizeof(player_dsc));
        player_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        player_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        player_dsc.header.w = (uint32_t)w;
        player_dsc.header.h = (uint32_t)h;
        player_dsc.header.stride = (uint32_t)w * 2;
        player_dsc.data_size = (uint32_t)(w * h * 2);
        player_total = qzcam_player_total_frames();
        lv_bar_set_value(player_bar, 0, LV_ANIM_OFF);

        lv_obj_remove_flag(player_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(player_screen);

        int period = 1000 / (qzcam_player_fps() > 0 ? qzcam_player_fps() : 15);
        if (period < 20) period = 20;
        player_playing = true;
        player_timer = lv_timer_create(player_tick, (uint32_t)period, NULL);
        return;
    }
    /* 照片：全屏看（LVGL 自带 BMP 解码 + FS_STDIO，路径用 A: 前缀） */
    lv_image_set_src(viewer_image, path);
    lv_image_set_inner_align(viewer_image, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(viewer);
}

static int compare_entry_desc(const void *a, const void *b)
{
    const gallery_entry_t *x = a;
    const gallery_entry_t *y = b;
    return strcmp(y->name, x->name);       /* 文件名里带时间戳，倒序 = 最新在前 */
}

static void gallery_refresh(void)
{
    if (!gallery_list) return;
    lv_obj_clean(gallery_list);

    /* 静态数组：行上的回调长期持有这些指针，所以不能是"每次刷新就 free"的堆内存 */
    static gallery_entry_t entries[50];

    DIR *dir = opendir(qzcam_storage_dir());
    if (!dir) {
        lv_obj_t *label = make_label(gallery_list, "打不开存储目录", 13, COLOR_DIM);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);
        return;
    }

    int count = 0;
    struct dirent *item;
    while (count < 50 && (item = readdir(dir)) != NULL) {
        const char *name = item->d_name;
        bool video = strstr(name, ".avi") != NULL;
        bool photo = strstr(name, ".bmp") != NULL;
        if (!video && !photo) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", qzcam_storage_dir(), name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        snprintf(entries[count].name, sizeof(entries[count].name), "%.79s", name);
        entries[count].size = (unsigned long long)st.st_size;
        entries[count].video = video;
        count++;
    }
    closedir(dir);

    if (count == 0) {
        lv_obj_t *label = make_label(gallery_list, "还没有照片或录像", 13, COLOR_DIM);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 8);
        return;
    }
    qsort(entries, (size_t)count, sizeof(gallery_entry_t), compare_entry_desc);

    for (int i = 0; i < count; i++) {
        char text[200];
        /* 图标字形在中文字体里没有，用中文标签代替（符号字体只对 ASCII 图标有效） */
        snprintf(text, sizeof(text), "%s %.60s  %.1f%s",
                 entries[i].video ? "影" : "图",
                 entries[i].name,
                 entries[i].size >= (1u << 20) ? (double)entries[i].size / (double)(1u << 20) : (double)entries[i].size / 1024.0,
                 entries[i].size >= (1u << 20) ? "MB" : "KB");

        lv_obj_t *row = lv_button_create(gallery_list);
        lv_obj_set_size(row, qzcam_panel_w - 16, 26);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, i * 28);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A20), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_t *label = make_label(row, text, 12, COLOR_TEXT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_add_event_cb(row, gallery_item_cb, LV_EVENT_CLICKED, &entries[i]);
    }
}

static void gallery_open_cb(lv_event_t *event)
{
    (void)event;
    gallery_refresh();
    lv_obj_remove_flag(gallery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(gallery);
}

/* ------------------------------------------------------------------ 交互 */

static void persist(void)
{
    settings_dirty = true;   /* 由 preview_timer 兜底落盘（节流） */
}

/* 拍摄模式（settings.mode）→ 界面行为：只有 视频/照片 真正改变快门行为，
 * 慢动作/延时/循环 目前按视频处理（采集管线同一套），等 VENC 接入再分化。 */
static capture_mode_t mode_to_capture(int mode)
{
    return (mode == 1) ? MODE_PHOTO : MODE_VIDEO;
}

static void update_mode_ui(void)
{
    const qzcam_settings_t *s = qzcam_settings();
    capture_mode = mode_to_capture(s->mode);

    /* 快门：白环 + 内圆（视频=红，照片=白） */
    if (shutter_core)
        lv_obj_set_style_bg_color(shutter_core, capture_mode == MODE_VIDEO ? COLOR_REC : COLOR_SHUTTER, 0);

    /* 模式卡片高亮（选择器开着时才可见） */
    for (int i = 0; i < 5; i++) {
        if (!mode_cards[i]) continue;
        bool on = (s->mode == i);
        lv_obj_set_style_border_color(mode_cards[i], on ? COLOR_ACCENT : lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_border_width(mode_cards[i], on ? 2 : 1, 0);
        lv_obj_set_style_bg_color(mode_cards[i], on ? COLOR_CARD_2 : COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(mode_cards[i], on ? LV_OPA_COVER : LV_OPA_70, 0);
    }
}

/* 面板滑入/滑出：同一条路（skills/apple-design §7 空间一致），
 * 250ms ease-out 进、200ms ease-out 出，全程可被打断（LVGL anim 可删可替）。 */
static void sheet_slide_in(lv_obj_t *sheet)
{
    int h = lv_obj_get_height(sheet);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sheet);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sheet);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, qzcam_panel_h, qzcam_panel_h - h);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void sheet_slide_out(lv_obj_t *sheet)
{
    int h = lv_obj_get_height(sheet);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sheet);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, qzcam_panel_h - h, qzcam_panel_h);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, sheet_hide_cb);
    lv_anim_start(&a);
}

static void shutter_press_anim(bool pressed)
{
    if (!shutter_button) return;
    /* 按压反馈在按下瞬间给出（skills/apple-design §1），100~160ms */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, shutter_button);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_transform_scale);
    lv_anim_set_values(&a, pressed ? 256 : 235, pressed ? 235 : 256);
    lv_anim_set_duration(&a, pressed ? 100 : 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void mode_card_cb(lv_event_t *event)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(event);
    qzcam_settings_t *s = qzcam_settings();
    if (s->mode != idx) {
        s->mode = idx;
        if (qzcam_rec_active()) {
            qzcam_rec_stop();
            toast("已停止录像");
        }
        update_mode_ui();
        persist();
    }
    char text[96];
    snprintf(text, sizeof(text), "%s · %s", mode_titles[idx], mode_specs[idx]);
    toast(text);
    sheet_slide_out(mode_sheet);
}

static void mode_open_cb(lv_event_t *event)
{
    (void)event;
    update_mode_ui();
    sheet_slide_in(mode_sheet);
}

static void mode_close_cb(lv_event_t *event)
{
    (void)event;
    sheet_slide_out(mode_sheet);
}

static void settings_open_cb(lv_event_t *event)
{
    (void)event;
    sheet_slide_in(settings_page);
}

static void settings_close_cb(lv_event_t *event)
{
    (void)event;
    sheet_slide_out(settings_page);
}

static void mode_cb(lv_event_t *event)
{
    (void)event;
    /* 兼容旧入口（自检钩子）：0=拍照 1=录像 */
    qzcam_settings_t *s = qzcam_settings();
    s->mode = (capture_mode == MODE_VIDEO) ? 1 : 0;
    update_mode_ui();
    persist();
}

static void capture_one_photo(void);
static void photo_step_tick(lv_timer_t *timer);

static void shutter_cb(lv_event_t *event)
{
    (void)event;

    if (capture_mode == MODE_VIDEO) {
        if (qzcam_rec_active()) {
            qzcam_rec_stop();
            char text[128];
            snprintf(text, sizeof(text), "已保存录像（%d 帧 %.1fMB）",
                     qzcam_rec_frames(), (double)qzcam_rec_bytes() / (double)(1u << 20));
            toast(text);
            gallery_refresh();
        } else {
            char path[240];
            if (qzcam_rec_start(path, sizeof(path))) {
                toast("开始录像");
            } else {
                toast("录像启动失败（存储不可写？）");
            }
        }
        update_storage_label();
        return;
    }

    /* 拍照 */
    const qzcam_settings_t *ps = qzcam_settings();
    if (capture_timer) return;              /* 倒计时/连拍进行中，忽略快门 */

    if (ps->selfie_sec > 0) {               /* 定时自拍：先倒计时 */
        step_mode = 1;
        step_left = ps->selfie_sec;
        capture_timer = lv_timer_create(photo_step_tick, 1000, NULL);
        return;
    }
    if (ps->burst > 1) {                    /* 连拍：首张立刻，后续定时 */
        capture_one_photo();
        step_mode = 2;
        step_left = ps->burst - 1;
        capture_timer = lv_timer_create(photo_step_tick, 350, NULL);
        return;
    }
    capture_one_photo();
}

/* 单张拍摄：白闪 + 存图 + 提示（自拍/连拍/普通拍照共用） */
static void capture_one_photo(void)
{
    lv_obj_set_style_opa(flash, LV_OPA_60, 0);
    lv_timer_create(flash_off, 110, NULL);

    char path[240];
    if (qzcam_cam_snapshot(path, sizeof(path))) {
        const char *name = strrchr(path, '/');
        toast(name ? name + 1 : path);
        gallery_refresh();
    } else {
        toast(qzcam_cam_status());
    }
    update_storage_label();
}

/* 自拍倒计时 / 连拍的节拍器 */
static void photo_step_tick(lv_timer_t *timer)
{
    if (step_mode == 1) {
        if (step_left > 0) {
            lv_label_set_text_fmt(countdown_label, "%d", step_left);
            lv_obj_remove_flag(countdown_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(countdown_label);
            step_left--;
            return;
        }
        lv_obj_add_flag(countdown_label, LV_OBJ_FLAG_HIDDEN);
        const qzcam_settings_t *ps = qzcam_settings();
        if (ps->burst > 1) {                /* 倒计时结束 → 继续连拍 */
            step_mode = 2;
            step_left = ps->burst;
            lv_timer_set_period(timer, 350);
            return;
        }
        capture_one_photo();
        lv_timer_del(timer);
        capture_timer = NULL;
        step_mode = 0;
        return;
    }

    /* 连拍：每 350ms 一张 */
    capture_one_photo();
    step_left--;
    if (step_left <= 0) {
        lv_timer_del(timer);
        capture_timer = NULL;
        step_mode = 0;
        toast("连拍完成");
        gallery_refresh();
    }
}

/* 按压反馈：按下瞬间缩到 0.92，松开弹回（skills/animate：按钮反馈 100~160ms） */
static void shutter_pressed_cb(lv_event_t *event)
{
    (void)event;
    shutter_press_anim(true);
}

static void shutter_released_cb(lv_event_t *event)
{
    (void)event;
    shutter_press_anim(false);
}

static void drawer_cb(lv_event_t *event)
{
    (void)event;
    if (lv_obj_has_flag(drawer, LV_OBJ_FLAG_HIDDEN)) sheet_slide_in(drawer);
    else sheet_slide_out(drawer);
}

static void switch_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    bool *flag = (bool *)lv_event_get_user_data(event);
    *flag = lv_obj_has_state(target, LV_STATE_CHECKED);
    apply_overlays();
    update_mode_ui();
    persist();
}

static void slider_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    int *value = (int *)lv_event_get_user_data(event);
    *value = (int)lv_slider_get_value(target);
    persist();
}

static void reset_cb(lv_event_t *event)
{
    (void)event;
    qzcam_settings_reset();
    apply_overlays();
    update_mode_ui();
    persist();
    toast("已恢复默认设置");
}

static void close_cb(lv_event_t *event)
{
    (void)event;
    sheet_slide_out(drawer);
}

/* -------------------------------------------------------------- 部件构建 */

static lv_obj_t *make_switch_row(lv_obj_t *parent, int y, const char *title, bool *flag)
{
    lv_obj_t *label = make_label(parent, title, 12, COLOR_TEXT);
    lv_obj_set_pos(label, 12, y + 5);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 34, 18);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -12, y + 3);
    lv_obj_set_style_bg_color(sw, COLOR_CARD_2, 0);
    lv_obj_set_style_bg_color(sw, COLOR_OK, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (*flag) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, flag);
    return sw;
}

static lv_obj_t *make_slider_row(lv_obj_t *parent, int y, const char *title,
                                 int min, int max, int *value)
{
    lv_obj_t *label = make_label(parent, title, 12, COLOR_DIM);
    lv_obj_set_pos(label, 12, y + 4);

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, qzcam_panel_w - 150, 8);
    lv_obj_align(slider, LV_ALIGN_TOP_RIGHT, -52, y + 7);
    lv_slider_set_value(slider, *value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, COLOR_CARD_2, 0);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, value);
    return slider;
}

/* ---- 参数面板：四根滑杆 + 实时数值 ---- */

static lv_obj_t *sheet_value_labels[4];   /* 0=EV 1=色温 2=锐度 3=降噪 */

static void sheet_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    int idx = (int)(intptr_t)lv_event_get_user_data(event);
    int v = (int)lv_slider_get_value(slider);
    qzcam_settings_t *s = qzcam_settings();
    char text[24];
    switch (idx) {
    case 0:  s->ev = v;        snprintf(text, sizeof(text), "%+.1f", v / 10.0); break;
    case 1:  s->wb_kelvin = v; snprintf(text, sizeof(text), "%dK", v); break;
    case 2:  s->sharpness = v; snprintf(text, sizeof(text), "%d", v); break;
    default: s->denoise = v;   snprintf(text, sizeof(text), "%d", v); break;
    }
    lv_label_set_text(sheet_value_labels[idx], text);
    persist();
}

static void sheet_sync_values(void)
{
    const qzcam_settings_t *s = qzcam_settings();
    char text[24];
    /* 面板是逐行建的：后面的标签还不存在，跳过（建到哪行刷哪行） */
    snprintf(text, sizeof(text), "%+.1f", s->ev / 10.0);
    if (sheet_value_labels[0]) lv_label_set_text(sheet_value_labels[0], text);
    snprintf(text, sizeof(text), "%dK", s->wb_kelvin);
    if (sheet_value_labels[1]) lv_label_set_text(sheet_value_labels[1], text);
    snprintf(text, sizeof(text), "%d", s->sharpness);
    if (sheet_value_labels[2]) lv_label_set_text(sheet_value_labels[2], text);
    snprintf(text, sizeof(text), "%d", s->denoise);
    if (sheet_value_labels[3]) lv_label_set_text(sheet_value_labels[3], text);
}

static void make_sheet_slider_row(lv_obj_t *parent, int y, const char *title,
                                  int idx, int min, int max, int step)
{
    qzcam_settings_t *s = qzcam_settings();
    int value = (idx == 0) ? s->ev : (idx == 1) ? s->wb_kelvin
              : (idx == 2) ? s->sharpness : s->denoise;

    lv_obj_t *name = make_label(parent, title, 12, COLOR_TEXT);
    lv_obj_set_pos(name, 12, y + 5);

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, 78, y + 8);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, COLOR_CARD_2, 0);
    lv_obj_set_style_bg_color(slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, sheet_slider_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)idx);

    sheet_value_labels[idx] = make_label(parent, "", 12, COLOR_ACCENT);
    lv_obj_align(sheet_value_labels[idx], LV_ALIGN_TOP_RIGHT, -14, y + 5);
}

static void build_grid(lv_obj_t *screen)
{
    int w = qzcam_panel_w;
    int h = qzcam_panel_h;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *vertical = lv_obj_create(screen);
        lv_obj_remove_flag(vertical, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(vertical, 1, h);
        lv_obj_set_pos(vertical, w * (i + 1) / 3, 0);
        lv_obj_set_style_bg_color(vertical, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(vertical, LV_OPA_30, 0);
        lv_obj_set_style_border_width(vertical, 0, 0);
        lv_obj_set_style_radius(vertical, 0, 0);
        grid_lines[i] = vertical;

        lv_obj_t *horizontal = lv_obj_create(screen);
        lv_obj_remove_flag(horizontal, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(horizontal, w, 1);
        lv_obj_set_pos(horizontal, 0, h * (i + 1) / 3);
        lv_obj_set_style_bg_color(horizontal, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(horizontal, LV_OPA_30, 0);
        lv_obj_set_style_border_width(horizontal, 0, 0);
        lv_obj_set_style_radius(horizontal, 0, 0);
        grid_lines[2 + i] = horizontal;
    }
}

/* 参数调节：底部弹出面板（skills/apple-design §12 材质：实色深卡片，
 * 弱化边框；320 宽下"毛玻璃"退化为高不透明度深色，保证可读性） */
static void build_drawer(lv_obj_t *screen)
{
    int height = 150;
    drawer = lv_obj_create(screen);
    lv_obj_set_size(drawer, qzcam_panel_w, height);
    lv_obj_set_pos(drawer, 0, qzcam_panel_h);   /* 初始在屏幕外，打开时滑入 */
    lv_obj_set_style_bg_color(drawer, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(drawer, LV_OPA_90, 0);
    lv_obj_set_style_border_width(drawer, 0, 0);
    lv_obj_set_style_radius(drawer, 16, 0);
    lv_obj_set_style_pad_all(drawer, 0, 0);
    lv_obj_set_scrollbar_mode(drawer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(drawer, LV_OBJ_FLAG_HIDDEN);

    /* 顶部拖拽指示条（视觉上说明"这是个面板"） */
    lv_obj_t *grip = lv_obj_create(drawer);
    lv_obj_remove_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(grip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(grip, 36, 3);
    lv_obj_align(grip, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(grip, lv_color_hex(0x555559), 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_set_style_radius(grip, 2, 0);

    lv_obj_t *title = make_label(drawer, "参数调节", 13, COLOR_TEXT);
    lv_obj_set_pos(title, 12, 10);
    lv_obj_t *sub = make_label(drawer, "EV / 色温 / 锐度 / 降噪", 10, COLOR_DIM);
    lv_obj_set_pos(sub, 12, 26);

    lv_obj_t *done = make_button(drawer, "完成", 48, 20, 10);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_set_style_bg_color(done, COLOR_ACCENT, 0);
    lv_obj_add_event_cb(done, close_cb, LV_EVENT_CLICKED, NULL);

    int y = 44;
    make_sheet_slider_row(drawer, y, "曝光补偿", 0, -30, 30, 3); y += 26;
    make_sheet_slider_row(drawer, y, "白平衡", 1, 2500, 8000, 100); y += 26;
    make_sheet_slider_row(drawer, y, "锐度", 2, 0, 100, 1); y += 26;
    make_sheet_slider_row(drawer, y, "降噪", 3, 0, 100, 1); y += 26;

    lv_obj_t *reset = make_button(drawer, "重置", 48, 20, 10);
    lv_obj_set_pos(reset, 12, y - 2);
    lv_obj_set_style_bg_color(reset, COLOR_CARD_2, 0);
    lv_obj_add_event_cb(reset, reset_cb, LV_EVENT_CLICKED, NULL);
}

/* 模式选择：横向卡片（视频/照片/慢动作/延时/循环），选中橙色描边 */
static void build_modes(lv_obj_t *screen)
{
    mode_sheet = lv_obj_create(screen);
    lv_obj_set_size(mode_sheet, qzcam_panel_w, 132);
    lv_obj_set_pos(mode_sheet, 0, qzcam_panel_h);
    lv_obj_set_style_bg_color(mode_sheet, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(mode_sheet, LV_OPA_90, 0);
    lv_obj_set_style_border_width(mode_sheet, 0, 0);
    lv_obj_set_style_radius(mode_sheet, 16, 0);
    lv_obj_set_style_pad_all(mode_sheet, 0, 0);
    lv_obj_add_flag(mode_sheet, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(mode_sheet, "拍摄模式", 13, COLOR_TEXT);
    lv_obj_set_pos(title, 12, 8);
    lv_obj_t *close = make_button(mode_sheet, "收起", 44, 18, 9);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_obj_set_style_bg_color(close, COLOR_CARD_2, 0);
    lv_obj_add_event_cb(close, mode_close_cb, LV_EVENT_CLICKED, NULL);

    /* 卡片横向滚动；每张 86 宽，5 张总宽 440 > 320 → 可横向划 */
    lv_obj_t *row = lv_obj_create(mode_sheet);
    lv_obj_set_size(row, qzcam_panel_w, 96);
    lv_obj_set_pos(row, 0, 30);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_snap_x(row, LV_SCROLL_SNAP_START);

    for (int i = 0; i < 5; i++) {
        lv_obj_t *card = lv_obj_create(row);
        lv_obj_set_size(card, 86, 82);
        lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(card, mode_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        mode_cards[i] = card;

        /* 卡片上不叠图标字：中文字体没有线性图标字形，单字图标会跟模式名
         * 挤在一起读成"影视频"。名字放大居中就够干净。 */
        lv_obj_t *name = make_label(card, mode_titles[i], 18, COLOR_TEXT);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 22);
        lv_obj_t *spec = make_label(card, mode_specs[i], 9, COLOR_DIM);
        lv_obj_align(spec, LV_ALIGN_TOP_MID, 0, 58);
    }
}

/* 设置页：iOS 分组列表（全部落 cam_settings 并持久化） */
typedef struct {
    int *value;
    const char *const *labels;
    const int *values;
    int count;
    lv_obj_t *label;
} cycle_row_t;
static cycle_row_t cycle_rows[16];
static int cycle_rows_used;

static const int cycle_res[]   = { 0, 1, 2 };
static const char *const cycle_res_names[]   = { "2304×1296", "1920×1080", "1280×720" };
/* 帧率档：标准模式（SC3336 2304x1296@30）。社区 75fps 驱动实测
 * "传感器出流正常但 rkaiq 主通路全黑"（LuckfoxTECH/luckfox-pico#67），
 * 暂不上 —— 驱动/IQ 文件仍留在镜像里（lib/camera/sc3336_75.ko），
 * 要再试时参考 git 历史。 */
static const int cycle_fps[]   = { 30, 25, 15 };
static const char *const cycle_fps_names[]   = { "30fps", "25fps", "15fps" };
static const int cycle_codec[] = { 0, 1 };
static const char *const cycle_codec_names[] = { "H.265", "H.264" };
static const int cycle_br[]    = { 0, 1, 2 };
static const char *const cycle_br_names[]    = { "智能编码", "CBR", "VBR" };
static const int cycle_nr[]    = { 0, 1, 2, 3 };
static const char *const cycle_nr_names[]    = { "关闭", "低", "中", "高" };
static const int cycle_wb[]    = { 0, 1, 2, 3, 4 };
static const char *const cycle_wb_names[]    = { "自动", "日光", "阴天", "白炽灯", "手动" };
static const int cycle_loop[]  = { 60, 180, 300, 600 };
static const char *const cycle_loop_names[]  = { "1min", "3min", "5min", "10min" };
static const int cycle_off[]   = { 0, 60, 180, 300 };
static const char *const cycle_off_names[]   = { "从不", "1min", "3min", "5min" };
static const int cycle_selfie[] = { 0, 3, 5, 10 };
static const char *const cycle_selfie_names[] = { "关", "3秒", "5秒", "10秒" };
static const int cycle_burst[]  = { 1, 3, 5, 10 };
static const char *const cycle_burst_names[]  = { "单张", "3张", "5张", "10张" };

static void cycle_row_cb(lv_event_t *event)
{
    cycle_row_t *r = &cycle_rows[(int)(intptr_t)lv_event_get_user_data(event)];
    int idx = 0;
    for (int i = 0; i < r->count; i++)
        if (*r->value == r->values[i]) { idx = i; break; }
    int old = *r->value;
    idx = (idx + 1) % r->count;
    *r->value = r->values[idx];
    lv_label_set_text(r->label, r->labels[idx]);
    /* 帧率 >30 要换传感器驱动（高速模式），只能在下次开机生效 —— 明说，
     * 别让人以为立刻切过去了。 */
    if (r->value == &qzcam_settings()->rec_fps) {
        bool hi_new = *r->value > 30, hi_old = old > 30;
        if (hi_new != hi_old)
            toast(hi_new ? "高速模式：重启后生效" : "标准模式：重启后生效");
    }
    update_mode_ui();
    persist();
}

static void make_cycle_row(lv_obj_t *parent, int y, const char *title,
                           int *value, const int *values,
                           const char *const *labels, int count)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, qzcam_panel_w - 16, 24);
    lv_obj_set_pos(row, 4, y);
    lv_obj_set_style_bg_color(row, COLOR_CARD, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *name = make_label(row, title, 12, COLOR_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);

    cycle_row_t *r = &cycle_rows[cycle_rows_used++];
    r->value = value;
    r->values = values;
    r->labels = labels;
    r->count = count;
    r->label = make_label(row, "", 12, COLOR_ACCENT);
    lv_obj_align(r->label, LV_ALIGN_RIGHT_MID, -8, 0);
    int idx = 0;
    for (int i = 0; i < count; i++)
        if (*value == values[i]) { idx = i; break; }
    lv_label_set_text(r->label, labels[idx]);

    lv_obj_add_event_cb(row, cycle_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(cycle_rows_used - 1));
}

static void make_group_header(lv_obj_t *parent, int y, const char *title)
{
    lv_obj_t *label = make_label(parent, title, 10, COLOR_DIM);
    lv_obj_set_pos(label, 12, y);
}

/* 设置页整屏：滑动的分组列表 */
static void build_settings(lv_obj_t *screen)
{
    settings_page = lv_obj_create(screen);
    lv_obj_set_size(settings_page, qzcam_panel_w, qzcam_panel_h);
    lv_obj_set_pos(settings_page, 0, qzcam_panel_h);
    lv_obj_set_style_bg_color(settings_page, COLOR_SCR, 0);
    lv_obj_set_style_bg_opa(settings_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_page, 0, 0);
    lv_obj_set_style_radius(settings_page, 0, 0);
    lv_obj_set_style_pad_all(settings_page, 0, 0);
    lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = make_label(settings_page, "设置", 16, COLOR_TEXT);
    lv_obj_set_pos(title, 12, 8);
    lv_obj_t *close = make_button(settings_page, "完成", 48, 20, 10);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_set_style_bg_color(close, COLOR_ACCENT, 0);
    lv_obj_add_event_cb(close, settings_close_cb, LV_EVENT_CLICKED, NULL);

    /* 滚动内容层：所有行都放这里 */
    lv_obj_t *list = lv_obj_create(settings_page);
    lv_obj_set_size(list, qzcam_panel_w, qzcam_panel_h - 34);
    lv_obj_set_pos(list, 0, 34);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    qzcam_settings_t *s = qzcam_settings();
    int y = 4;
    make_group_header(list, y, "拍摄"); y += 16;
    make_cycle_row(list, y, "定时自拍", &s->selfie_sec, cycle_selfie, cycle_selfie_names, 4); y += 26;
    make_cycle_row(list, y, "连拍张数", &s->burst, cycle_burst, cycle_burst_names, 4); y += 26;
    make_group_header(list, y, "录制"); y += 16;
    make_cycle_row(list, y, "分辨率", &s->resolution, cycle_res, cycle_res_names, 3); y += 26;
    make_cycle_row(list, y, "帧率", &s->rec_fps, cycle_fps, cycle_fps_names, 3); y += 26;
    make_cycle_row(list, y, "编码格式", &s->codec, cycle_codec, cycle_codec_names, 2); y += 26;
    make_cycle_row(list, y, "码率控制", &s->bitrate_mode, cycle_br, cycle_br_names, 3); y += 26;
    make_switch_row(list, y, "电子防抖", &s->eis); y += 30;

    make_group_header(list, y, "图像"); y += 16;
    make_switch_row(list, y, "HDR", &s->hdr); y += 30;
    make_switch_row(list, y, "宽动态 WDR", &s->wdr); y += 30;
    make_cycle_row(list, y, "降噪", &s->isp_denoise, cycle_nr, cycle_nr_names, 4); y += 26;
    make_cycle_row(list, y, "白平衡模式", &s->wb_mode, cycle_wb, cycle_wb_names, 5); y += 26;
    make_switch_row(list, y, "夜视增强", &s->night_mode); y += 30;

    make_group_header(list, y, "音频"); y += 16;
    make_switch_row(list, y, "录音", &s->audio_rec); y += 30;
    make_switch_row(list, y, "AI 降噪", &s->audio_denoise); y += 30;

    make_group_header(list, y, "循环录制"); y += 16;
    make_cycle_row(list, y, "分段时长", &s->loop_seconds, cycle_loop, cycle_loop_names, 4); y += 26;
    make_switch_row(list, y, "循环覆盖", &s->loop_overwrite); y += 30;

    make_group_header(list, y, "通用"); y += 16;
    make_switch_row(list, y, "日期水印", &s->watermark); y += 30;
    make_switch_row(list, y, "时间戳", &s->timestamp); y += 30;
    make_switch_row(list, y, "网格线", &s->grid); y += 30;
    make_cycle_row(list, y, "自动关机", &s->auto_off, cycle_off, cycle_off_names, 4); y += 30;
    make_switch_row(list, y, "镜像", &s->mirror); y += 34;

    lv_obj_t *reset = make_button(list, "恢复默认设置", 120, 24, 10);
    lv_obj_align(reset, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(reset, COLOR_CARD_2, 0);
    lv_obj_add_event_cb(reset, reset_cb, LV_EVENT_CLICKED, NULL);
}

static void build_gallery(lv_obj_t *screen)
{
    gallery = lv_obj_create(screen);
    lv_obj_remove_flag(gallery, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(gallery, qzcam_panel_w, qzcam_panel_h);
    lv_obj_set_pos(gallery, 0, 0);
    lv_obj_set_style_bg_color(gallery, lv_color_hex(0x0E0E12), 0);
    lv_obj_set_style_bg_opa(gallery, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gallery, 0, 0);
    lv_obj_set_style_radius(gallery, 0, 0);
    lv_obj_set_style_pad_all(gallery, 0, 0);
    lv_obj_add_flag(gallery, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bar = make_bar(gallery, 0, 24);
    lv_obj_t *title = make_label(bar, "相册", 14, COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *close = make_button(bar, "返回", 52, 20, 6);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_event_cb(close, gallery_close_cb, LV_EVENT_CLICKED, NULL);
    /* 存储余量放相册顶栏（预览画面上不再常驻） */
    storage_label = make_label(bar, "", 9, COLOR_DIM);
    lv_obj_align(storage_label, LV_ALIGN_RIGHT_MID, -64, 0);

    gallery_list = lv_obj_create(gallery);
    lv_obj_set_size(gallery_list, qzcam_panel_w, qzcam_panel_h - 24);
    lv_obj_set_pos(gallery_list, 0, 24);
    lv_obj_set_style_bg_opa(gallery_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gallery_list, 0, 0);
    lv_obj_set_style_pad_all(gallery_list, 4, 0);
    lv_obj_set_style_radius(gallery_list, 0, 0);
    lv_obj_set_scrollbar_mode(gallery_list, LV_SCROLLBAR_MODE_AUTO);

    /* 照片全屏查看 */
    viewer = lv_obj_create(screen);
    lv_obj_remove_flag(viewer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(viewer, qzcam_panel_w, qzcam_panel_h);
    lv_obj_set_pos(viewer, 0, 0);
    lv_obj_set_style_bg_color(viewer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(viewer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(viewer, 0, 0);
    lv_obj_set_style_radius(viewer, 0, 0);
    lv_obj_set_style_pad_all(viewer, 0, 0);
    lv_obj_add_flag(viewer, LV_OBJ_FLAG_HIDDEN);

    viewer_image = lv_image_create(viewer);
    lv_obj_set_size(viewer_image, qzcam_panel_w, qzcam_panel_h);
    lv_obj_set_pos(viewer_image, 0, 0);

    lv_obj_t *back = make_button(viewer, "返回", 52, 22, 6);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_add_event_cb(back, viewer_close_cb, LV_EVENT_CLICKED, NULL);

    /* ---- 视频播放器（录像 = 未压缩 RGB565 AVI，板上直接放）---- */
    player_screen = lv_obj_create(screen);
    lv_obj_remove_flag(player_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(player_screen, qzcam_panel_w, qzcam_panel_h);
    lv_obj_set_pos(player_screen, 0, 0);
    lv_obj_set_style_bg_color(player_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(player_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(player_screen, 0, 0);
    lv_obj_set_style_radius(player_screen, 0, 0);
    lv_obj_set_style_pad_all(player_screen, 0, 0);
    lv_obj_add_flag(player_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(player_screen, player_area_cb, LV_EVENT_CLICKED, NULL);

    /* 进度条的点击热区（条本身只有 4px 高，专门给一层 20px 的透明点击条） */
    lv_obj_t *seek_strip = lv_obj_create(player_screen);
    lv_obj_remove_flag(seek_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(seek_strip, qzcam_panel_w, 22);
    lv_obj_align(seek_strip, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_opa(seek_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(seek_strip, 0, 0);
    lv_obj_add_event_cb(seek_strip, player_seek_strip_cb, LV_EVENT_CLICKED, NULL);

    player_image = lv_image_create(player_screen);
    lv_obj_set_size(player_image, qzcam_panel_w, qzcam_panel_h);
    lv_obj_set_pos(player_image, 0, 0);

    lv_obj_t *pback = make_button(player_screen, "返回", 52, 22, 6);
    lv_obj_align(pback, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_add_event_cb(pback, player_close_cb, LV_EVENT_CLICKED, NULL);

    player_time = make_label(player_screen, "00:00 / 00:00", 12, COLOR_TEXT);
    lv_obj_align(player_time, LV_ALIGN_BOTTOM_MID, 0, -6);

    player_bar = lv_bar_create(player_screen);
    lv_obj_set_size(player_bar, qzcam_panel_w - 24, 4);
    lv_obj_align(player_bar, LV_ALIGN_BOTTOM_MID, 0, -24);
}

void qzcam_ui_open_gallery(void)
{
    gallery_refresh();
    lv_obj_remove_flag(gallery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(gallery);
}

void qzcam_ui_view_first_photo(void)
{
    /* 找出目录里最新的一张 BMP（文件名带时间戳，字典序最大的就是最新的） */
    DIR *dir = opendir(qzcam_storage_dir());
    if (!dir) return;
    char newest[80] = "";
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (!strstr(item->d_name, ".bmp")) continue;
        if (strcmp(item->d_name, newest) > 0)
            snprintf(newest, sizeof(newest), "%.79s", item->d_name);
    }
    closedir(dir);
    if (!newest[0]) {
        toast("相册里还没有照片");
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "A:%s/%s", qzcam_storage_dir(), newest);
    lv_image_set_src(viewer_image, path);
    lv_image_set_inner_align(viewer_image, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(viewer);
}

void qzcam_ui_set_mode(int video)
{
    capture_mode = video ? MODE_VIDEO : MODE_PHOTO;
    update_mode_ui();
}

/* ------------------------------------------------------------------ 组装 */

lv_obj_t *qzcam_ui_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, COLOR_SCR, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    preview_image = lv_image_create(screen);
    lv_obj_set_size(preview_image, qzcam_panel_w, qzcam_panel_h);
    lv_obj_set_pos(preview_image, 0, 0);
    lv_obj_set_style_bg_color(preview_image, COLOR_SCR, 0);

    build_grid(screen);

    /* 顶栏（h=20）：左=取景状态，右=实测帧率 */
    lv_obj_t *top = make_bar(screen, 0, 20);
    lv_obj_set_style_bg_opa(top, LV_OPA_60, 0);
    status_label = make_label(top, "取景未启动", 10, COLOR_DIM);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 6, 0);
    fps_label = make_label(top, "0.0 fps", 10, COLOR_DIM);
    lv_obj_align(fps_label, LV_ALIGN_RIGHT_MID, -6, 0);

    /* 录制状态胶囊：顶部居中，红点呼吸 + 计时（animate §状态指示） */
rec_capsule = lv_obj_create(screen);
    lv_obj_remove_flag(rec_capsule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(rec_capsule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rec_capsule, 92, 18);
    lv_obj_align(rec_capsule, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_color(rec_capsule, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(rec_capsule, LV_OPA_70, 0);
    lv_obj_set_style_border_width(rec_capsule, 1, 0);
    lv_obj_set_style_border_color(rec_capsule, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_radius(rec_capsule, 9, 0);
    lv_obj_set_style_pad_all(rec_capsule, 0, 0);
    lv_obj_add_flag(rec_capsule, LV_OBJ_FLAG_HIDDEN);

    rec_dot = lv_obj_create(rec_capsule);
    lv_obj_remove_flag(rec_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(rec_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rec_dot, 8, 8);
    lv_obj_align(rec_dot, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(rec_dot, COLOR_REC, 0);
    lv_obj_set_style_border_width(rec_dot, 0, 0);
    lv_obj_set_style_radius(rec_dot, LV_RADIUS_CIRCLE, 0);

    rec_time_label = make_label(rec_capsule, "00:00", 12, COLOR_TEXT);
    lv_obj_align(rec_time_label, LV_ALIGN_LEFT_MID, 22, 0);

    /* 预览画面保持干净：规格/增强项/存储都不再常驻（分别看模式抽屉、
     * 设置页、相册顶栏）。顶栏右侧 = 实测帧率 + 时间戳开关控制的小时钟。 */
    clock_label = make_label(top, "--:--:--", 10, COLOR_DIM);
    lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, -56, 0);

    /* 底部悬浮控制条：相册 / 大快门 / 模式 / 设置（圆角悬浮卡片） */
lv_obj_t *bottom = lv_obj_create(screen);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bottom, qzcam_panel_w - 8, 52);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(bottom, COLOR_BAR, 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_80, 0);
    lv_obj_set_style_border_width(bottom, 1, 0);
    lv_obj_set_style_border_color(bottom, lv_color_hex(0x2A2A2E), 0);
    lv_obj_set_style_radius(bottom, 22, 0);
    lv_obj_set_style_pad_all(bottom, 0, 0);

    lv_obj_t *gallery_button = make_button(bottom, "相册", 44, 32, 16);
    lv_obj_align(gallery_button, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(gallery_button, COLOR_CARD_2, 0);
    lv_obj_add_event_cb(gallery_button, gallery_open_cb, LV_EVENT_CLICKED, NULL);

    /* 大快门：白环 + 内圆（视频=红），按压缩放反馈 */
    shutter_button = lv_obj_create(bottom);
    lv_obj_remove_flag(shutter_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(shutter_button, 44, 44);
    lv_obj_align(shutter_button, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(shutter_button, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(shutter_button, 3, 0);
    lv_obj_set_style_border_color(shutter_button, COLOR_SHUTTER, 0);
    lv_obj_set_style_radius(shutter_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(shutter_button, 0, 0);
    lv_obj_add_event_cb(shutter_button, shutter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(shutter_button, shutter_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(shutter_button, shutter_released_cb, LV_EVENT_RELEASED, NULL);

    shutter_core = lv_obj_create(shutter_button);
    lv_obj_remove_flag(shutter_core, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(shutter_core, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(shutter_core, 30, 30);
    lv_obj_center(shutter_core);
    lv_obj_set_style_bg_color(shutter_core, COLOR_SHUTTER, 0);
    lv_obj_set_style_border_width(shutter_core, 0, 0);
    lv_obj_set_style_radius(shutter_core, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *mode_button = make_button(bottom, "模式", 44, 32, 16);
    lv_obj_align(mode_button, LV_ALIGN_RIGHT_MID, -62, 0);
    lv_obj_set_style_bg_color(mode_button, COLOR_CARD_2, 0);
    lv_obj_add_event_cb(mode_button, mode_open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *settings_button = make_button(bottom, "设置", 44, 32, 16);
    lv_obj_align(settings_button, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(settings_button, COLOR_CARD_2, 0);
    lv_obj_add_event_cb(settings_button, settings_open_cb, LV_EVENT_CLICKED, NULL);

    photo_label = make_label(screen, "照片 0", 10, COLOR_DIM);
    lv_obj_align(photo_label, LV_ALIGN_TOP_RIGHT, -10, 166);
    lv_obj_remove_flag(photo_label, LV_OBJ_FLAG_CLICKABLE);

    toast_label = make_label(screen, "", 12, COLOR_TEXT);
    lv_obj_remove_flag(toast_label, LV_OBJ_FLAG_CLICKABLE);   /* 提示条不挡点击 */
    lv_obj_set_style_bg_color(toast_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(toast_label, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(toast_label, 6, 0);
    lv_obj_set_style_pad_ver(toast_label, 3, 0);
    lv_obj_set_style_radius(toast_label, 6, 0);
    lv_obj_align(toast_label, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_add_flag(toast_label, LV_OBJ_FLAG_HIDDEN);

    /* 无信号浮层（放在 flash 之前创建：拍照白闪仍然压在最上面）：
     * 真机没出画时，这里直接把原因列出来 —— 相机的问题只在板子上，没串口
     * 的话屏幕就是唯一的"调试口"。 */
nosignal_panel = lv_obj_create(screen);
    lv_obj_remove_flag(nosignal_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(nosignal_panel, qzcam_panel_w - 16);
    lv_obj_set_height(nosignal_panel, LV_SIZE_CONTENT);
    lv_obj_align(nosignal_panel, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_bg_color(nosignal_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(nosignal_panel, LV_OPA_80, 0);
    lv_obj_set_style_border_color(nosignal_panel, COLOR_REC, 0);
    lv_obj_set_style_border_width(nosignal_panel, 1, 0);
    lv_obj_set_style_radius(nosignal_panel, 8, 0);
    lv_obj_set_style_pad_all(nosignal_panel, 8, 0);
    lv_obj_set_flex_flow(nosignal_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nosignal_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(nosignal_panel, 4, 0);

    make_label(nosignal_panel, "相机无信号", 14, COLOR_REC);
    nosignal_status = make_label(nosignal_panel, "", 12, COLOR_TEXT);
    lv_label_set_long_mode(nosignal_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nosignal_status, qzcam_panel_w - 34);
    nosignal_diag = make_label(nosignal_panel, "", 11, COLOR_DIM);
    lv_label_set_long_mode(nosignal_diag, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nosignal_diag, qzcam_panel_w - 34);
    lv_obj_add_flag(nosignal_panel, LV_OBJ_FLAG_HIDDEN);

flash = lv_obj_create(screen);
    lv_obj_remove_flag(flash, LV_OBJ_FLAG_SCROLLABLE);
    /* 关键：flash 盖在所有按钮之上。lv_obj 默认 CLICKABLE，不摘掉的话这层
     * 全屏"白闪"会把每一次触摸都吞掉 —— 现象就是"屏幕点不动"。 */
    lv_obj_remove_flag(flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(flash, qzcam_panel_w, qzcam_panel_h);
    lv_obj_set_pos(flash, 0, 0);
    lv_obj_set_style_bg_color(flash, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(flash, 0, 0);
    lv_obj_set_style_radius(flash, 0, 0);
    lv_obj_set_style_opa(flash, LV_OPA_TRANSP, 0);

    /* 自拍倒计时的大数字（创建在 flash 之后 = 盖在一切之上） */
    countdown_label = make_label(screen, "", 56, COLOR_ACCENT);
    lv_obj_align(countdown_label, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(countdown_label, LV_OBJ_FLAG_HIDDEN);

    build_drawer(screen);
    build_modes(screen);
    build_settings(screen);
    build_gallery(screen);

    qzcam_storage_refresh();
    apply_overlays();
    update_mode_ui();
    update_storage_label();
    lv_timer_create(preview_timer, 30, NULL);   /* 与 LV_DEF_REFR_PERIOD 对齐：错拍会造成预览忽快忽慢 */
    lv_timer_create(clock_timer, 1000, NULL);
    clock_timer(NULL);

    return screen;
}
