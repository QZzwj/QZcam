/* lv_render_probe —— 宿主侧渲染探针
 *
 * 把一张图片喂给 lv_image，抓 framebuffer 数像素，回答"LVGL 到底画没画出来"。
 * 用的是仓库里同一份 vendored LVGL + 同一份 lv_conf.h（LV_USE_TJPGD /
 * LV_USE_BMP / LV_USE_FS_STDIO / 软件渲染 全跟真机一致），所以 PC 上的结论
 * 可以直接搬到板子上 —— 这类问题光读代码是猜不出来的。
 *
 * 背景（2026-10 真机 bug）：照片查看器把 1296p 的 JPEG 塞进 320×240，
 * 一开始用 lv_image_set_scale() 缩小 → 屏幕上什么都没有（纯黑）。
 * 探针复现了这个行为：
 *   mode > 0（比如 35）：什么都没画（非背景 0/76800）
 *   mode = -2（现在的实现）：320×180 居中 + 上下留黑，画面正常
 * 结论是 LVGL 9.2 的软件缩放里只有"放大"（scale > 256）管用，"缩小"对从文件
 * 流式解码的源（JPEG/BMP）画不出来。
 *
 * 用法：见同目录 README.md
 */
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 240

static uint16_t fb[W * H];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int32_t w = lv_area_get_width(area);
    for (int32_t y = area->y1; y <= area->y2; y++)
        for (int32_t x = area->x1; x <= area->x2; x++) {
            uint16_t v = ((uint16_t *)px)[(y - area->y1) * w + (x - area->x1)];
            if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = v;
        }
    lv_display_flush_ready(disp);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("用法: lv_render_probe <图片> [模式]\n"
               "  模式 0    = 1:1（默认）\n"
               "  模式 -1   = STRETCH 铺满整个对象\n"
               "  模式 -2   = 按源比例给画面框 + STRETCH + 居中（viewer_show_photo 的做法）\n"
               "  模式 >0   = lv_image_set_scale(模式)，256 = 1:1\n");
        return 2;
    }
    const char *path = argv[1];
    int mode = argc > 2 ? atoi(argv[2]) : 0;

    lv_init();
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static uint8_t drawbuf[W * H * 2];
    lv_display_set_buffers(disp, drawbuf, NULL, sizeof(drawbuf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    /* 背景铺深蓝：这样"没画上去"和"图本身是黑"能分开 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000040), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *img = lv_image_create(scr);
    lv_obj_set_size(img, W, H);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_style_pad_all(img, 0, 0);

    char src[512];
    snprintf(src, sizeof(src), "A:%s", path);
    lv_image_header_t hdr;
    lv_result_t info = lv_image_decoder_get_info(src, &hdr);

    lv_image_set_src(img, src);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);

    int bw = W, bh = H;
    if (mode == -2) {
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
        if (info == LV_RESULT_OK && hdr.w && hdr.h) {
            if ((int32_t)hdr.w * H > (int32_t)hdr.h * W)
                bh = (int)W * (int)hdr.h / (int)hdr.w;                /* 源更宽：宽贴边 */
            else
                bw = (int)H * (int)hdr.w / (int)hdr.h;                /* 源更高：高贴边 */
        }
        lv_obj_set_size(img, bw, bh);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    } else if (mode == -1) {
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    } else if (mode > 0) {
        lv_image_set_scale(img, (uint32_t)mode);
    }

    printf("模式 %d  %s  源 %ux%u  画面框 %dx%d\n", mode,
           info == LV_RESULT_OK ? "解析OK" : "解析失败",
           (unsigned)hdr.w, (unsigned)hdr.h, bw, bh);

    memset(fb, 0, sizeof(fb));
    for (int i = 0; i < 40; i++) { lv_tick_inc(20); lv_timer_handler(); }

    const uint16_t bg16 = 0x0008;   /* lv_color_hex(0x000040) 的 RGB565 值 */
    long nonbg = 0;
    for (int i = 0; i < W * H; i++) if (fb[i] != bg16) nonbg++;

    int first = -1, last = -1;
    for (int y = 0; y < H; y++) {
        int n = 0;
        for (int x = 0; x < W; x++) if (fb[y * W + x] != bg16) n++;
        if (n > 0) { if (first < 0) first = y; last = y; }
    }
    printf("      非背景 %ld/%d   纵向画面 rows %d..%d\n", nonbg, W * H, first, last);

    FILE *f = fopen("out.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) {
            uint16_t v = fb[i];
            unsigned char rgb[3] = {
                (unsigned char)(((v >> 11) & 31) * 255 / 31),
                (unsigned char)(((v >> 5) & 63) * 255 / 63),
                (unsigned char)((v & 31) * 255 / 31)};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        printf("      已写出 out.ppm\n");
    }
    return 0;
}
