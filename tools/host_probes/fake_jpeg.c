/* fake_jpeg —— 给 jpeg_scale_test 用的假 libjpeg
 *
 * 不真解码：按 fake_src_w/h 造一张"能反推几何"的图案，并按 scale_num/scale_denom
 * 吐出来。用的是 SDK 里那份真 jpeglib.h（真结构体），所以测的是 cam_image.c 里
 * 那段真代码的取比例、留黑与行映射，而不是它的复制品。
 *
 * 图案：R = 横向渐变（0→255），G = 纵向 8 条带，B = 常数。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

int fake_src_w = 2304, fake_src_h = 1296;

static void fake_fail(j_common_ptr cinfo) { (void)cinfo; }

struct jpeg_error_mgr *jpeg_std_error(struct jpeg_error_mgr *err)
{
    memset(err, 0, sizeof(*err));
    err->error_exit = fake_fail;
    return err;
}

void jpeg_CreateDecompress(j_decompress_ptr cinfo, int version, size_t structsize)
{
    (void)version;
    (void)structsize;
    memset(cinfo, 0, sizeof(struct jpeg_decompress_struct));
    cinfo->image_width = (JDIMENSION)fake_src_w;
    cinfo->image_height = (JDIMENSION)fake_src_h;
    cinfo->output_scanline = 0;
}

void jpeg_stdio_src(j_decompress_ptr cinfo, FILE *infile) { (void)cinfo; (void)infile; }

int jpeg_read_header(j_decompress_ptr cinfo, boolean require_image)
{
    (void)cinfo; (void)require_image;
    return JPEG_HEADER_OK;
}

boolean jpeg_start_decompress(j_decompress_ptr cinfo)
{
    if (cinfo->scale_denom == 0) return FALSE;
    cinfo->output_width = cinfo->image_width / cinfo->scale_denom;
    cinfo->output_height = cinfo->image_height / cinfo->scale_denom;
    cinfo->output_components = 3;
    cinfo->output_scanline = 0;
    return TRUE;
}

JDIMENSION jpeg_read_scanlines(j_decompress_ptr cinfo, JSAMPARRAY buf, JDIMENSION max_lines)
{
    unsigned w = cinfo->output_width ? cinfo->output_width : 1;
    unsigned h = cinfo->output_height ? cinfo->output_height : 1;
    int y = (int)cinfo->output_scanline;
    JSAMPROW rp = buf[0];
    for (unsigned x = 0; x < cinfo->output_width; x++) {
        rp[x * 3 + 0] = (unsigned char)(x * 255 / w);
        rp[x * 3 + 1] = (unsigned char)((y * 8 / (int)h) * 32);
        rp[x * 3 + 2] = 128;
    }
    cinfo->output_scanline += max_lines;
    return max_lines;
}

boolean jpeg_finish_decompress(j_decompress_ptr cinfo) { (void)cinfo; return TRUE; }
void jpeg_destroy_decompress(j_decompress_ptr cinfo) { (void)cinfo; }
void jpeg_abort_decompress(j_decompress_ptr cinfo) { (void)cinfo; }

/* 编码侧的桩：同一个 .o 里有编码函数，链接需要符号（本测试不调用它们） */
void jpeg_CreateCompress(j_compress_ptr cinfo, int version, size_t structsize)
{
    (void)version; (void)structsize;
    memset(cinfo, 0, sizeof(struct jpeg_compress_struct));
}
void jpeg_stdio_dest(j_compress_ptr cinfo, FILE *outfile) { (void)cinfo; (void)outfile; }
void jpeg_set_defaults(j_compress_ptr cinfo) { (void)cinfo; }
void jpeg_set_quality(j_compress_ptr cinfo, int quality, boolean force_baseline)
{ (void)cinfo; (void)quality; (void)force_baseline; }
void jpeg_start_compress(j_compress_ptr cinfo, boolean write_all_tables)
{ (void)cinfo; (void)write_all_tables; }
JDIMENSION jpeg_write_scanlines(j_compress_ptr cinfo, JSAMPARRAY buf, JDIMENSION num_lines)
{ (void)cinfo; (void)buf; return num_lines; }
void jpeg_finish_compress(j_compress_ptr cinfo) { (void)cinfo; }
void jpeg_destroy_compress(j_compress_ptr cinfo) { (void)cinfo; }
