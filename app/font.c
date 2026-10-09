#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TTF 的候选路径：设备（随 QZcam 装进 oem）与开发机各一份。 */
static const char *const ttf_paths[] = {
    "/oem/usr/share/fonts/NanoTikBazHei-Bold.ttf",
    "/usr/share/fonts/NanoTikBazHei-Bold.ttf",
    "/home/jn/QZdesk/QZcam/assets/NanoTikBazHei-Bold.ttf",
    "/home/jn/QZdesk/NanoTikBazHei-Bold.ttf",
};

#define FONT_SLOTS 8

static void *ttf_data;
static size_t ttf_size;

static struct {
    int px;
    lv_font_t *font;
} slots[FONT_SLOTS];

static void load_ttf(void)
{
    for (unsigned i = 0; i < sizeof(ttf_paths) / sizeof(ttf_paths[0]); i++) {
        FILE *fp = fopen(ttf_paths[i], "rb");
        if (!fp) continue;
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            continue;
        }
        long size = ftell(fp);
        rewind(fp);
        if (size <= 0) {
            fclose(fp);
            continue;
        }
        void *data = malloc((size_t)size);
        if (!data) {
            fclose(fp);
            continue;
        }
        if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
            free(data);
            fclose(fp);
            continue;
        }
        fclose(fp);
        ttf_data = data;
        ttf_size = (size_t)size;
        fprintf(stderr, "QZcam: 中文字体 %s (%ld 字节)\n", ttf_paths[i], size);
        return;
    }
    fprintf(stderr, "QZcam: 没找到中文字体，界面文字退回内置 ASCII 字体\n");
}

void qzcam_font_init(void)
{
    load_ttf();
}

/* 内置的 Montserrat 字号表（lv_conf.h 里已启用）。 */
static const lv_font_t *montserrat_for(int px)
{
    static const int32_t sizes[] = {12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 40};
    static const lv_font_t *fonts[] = {
        &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
        &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_22,
        &lv_font_montserrat_24, &lv_font_montserrat_26, &lv_font_montserrat_28,
        &lv_font_montserrat_30, &lv_font_montserrat_32, &lv_font_montserrat_34,
        &lv_font_montserrat_36, &lv_font_montserrat_40,
    };
    const int count = (int)(sizeof(sizes) / sizeof(sizes[0]));
    int best = 0, best_delta = 0x7FFFFFFF;
    for (int i = 0; i < count; i++) {
        int delta = sizes[i] > px ? sizes[i] - px : px - sizes[i];
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return fonts[best];
}

const lv_font_t *qzcam_font(int px)
{
    if (px < 8) px = 8;
    if (px > 64) px = 64;
    if (!ttf_data) return montserrat_for(px);

    int free_slot = -1;
    for (int i = 0; i < FONT_SLOTS; i++) {
        if (slots[i].font && slots[i].px == px) return slots[i].font;
        if (!slots[i].font && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return montserrat_for(px);

    /* 直接用数据指针建字体：全局缓存关掉时这条路最省事（同 QZdesk 的做法）。 */
    lv_font_t *font = lv_tiny_ttf_create_data_ex(ttf_data, ttf_size, px,
                                                 LV_FONT_KERNING_NONE, 0);
    if (!font) return montserrat_for(px);
    slots[free_slot].px = px;
    slots[free_slot].font = font;
    return font;
}

const lv_font_t *qzcam_icon(int px)
{
    return montserrat_for(px);
}
