/* jpeg_scale_test —— 验证 qzcam_image_decode_jpeg_rgb565()（cam_image.c）的几何
 *
 * 为什么要有这个测试：照片查看器连着两轮在板上是黑屏（一次是 LVGL 缩放的坑，
 * 一次是取比例/留黑写错）。几何这种事在 PC 上用假解码器就能验全，不必上板。
 *
 * 三个用例：
 *   1296p（16:9） → 320×240 面板：画面框应为 320×180，上下各留 30 行黑
 *   4:3            → 铺满 320×240
 *   竖构图          → 画面框 120×240，左右各留 100 列黑
 * 每个用例都检查：画面行/列数、起始位置、以及横向渐变是否从 0 铺到满
 * （后者证明整幅源都映射进来了，不是被裁了一角）。
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

extern int fake_src_w, fake_src_h;
bool qzcam_image_decode_jpeg_rgb565(const char *path, uint8_t *out, int out_w, int out_h);

#define W 320
#define H 240
static uint8_t buf[W * H * 2];
static int fail;

static uint16_t px(int x, int y)
{
    return (uint16_t)(buf[(y * W + x) * 2] | (buf[(y * W + x) * 2 + 1] << 8));
}
static int r_of(uint16_t v) { return (v >> 11) & 31; }

static void span_rows(int *first, int *last)
{
    *first = -1; *last = -1;
    for (int y = 0; y < H; y++) {
        int n = 0;
        for (int x = 0; x < W; x++) if (px(x, y)) n++;
        if (n > 0) { if (*first < 0) *first = y; *last = y; }
    }
}

static void span_cols(int *first, int *last)
{
    *first = -1; *last = -1;
    for (int x = 0; x < W; x++) {
        int n = 0;
        for (int y = 0; y < H; y++) if (px(x, y)) n++;
        if (n > 0) { if (*first < 0) *first = x; *last = x; }
    }
}

static void chk(const char *what, int got, int want)
{
    printf("  %-4s %-32s 实际=%d 期望=%d\n", got == want ? "OK" : "FAIL", what, got, want);
    if (got != want) fail = 1;
}

static void run_case(const char *name, int src_w, int src_h, int want_rows, int want_cols)
{
    fake_src_w = src_w;
    fake_src_h = src_h;
    memset(buf, 0, sizeof(buf));
    printf("[%s] %dx%d → %dx%d\n", name, src_w, src_h, W, H);

    if (!qzcam_image_decode_jpeg_rgb565("fake.jpg", buf, W, H)) {
        printf("  FAIL 解码返回 false\n");
        fail = 1;
        return;
    }

    int f, l;
    span_rows(&f, &l);
    chk("纵向画面行数", l - f + 1, want_rows);
    chk("纵向起始行（留黑）", f, (H - want_rows) / 2);
    span_cols(&f, &l);
    chk("横向画面列数", l - f + 1, want_cols);
    chk("横向起始列（留黑）", f, (W - want_cols) / 2);
    /* 渐变：画面最左列 R 应为 0、最右列 R 应为满值（31） */
    chk("画面最左列 R", r_of(px((W - want_cols) / 2, (H - want_rows) / 2)), 0);
    chk("画面最右列 R", r_of(px((W + want_cols) / 2 - 1, (H - want_rows) / 2)), 31);
}

int main(void)
{
    run_case("1296p", 2304, 1296, 180, 320);
    run_case("4:3", 640, 480, 240, 320);
    run_case("竖构图", 480, 960, 240, 120);
    printf("\n%s\n", fail ? "有用例失败" : "全部通过");
    return fail;
}
