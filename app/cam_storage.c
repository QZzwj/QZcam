#include "cam_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* 候选目录 + 介质短名，按优先级自上而下 */
static const struct {
    const char *dir;
    const char *label;
} candidates[] = {
    { "/mnt/sdcard", "SD 卡" },
    { "/userdata/qzcam", "用户区" },
    { "/tmp/qzcam", "临时" },
};

static char dir[192];
static char label[24] = "未知";
static bool ready;

static bool writable(const char *path)
{
    char probe[224];
    snprintf(probe, sizeof(probe), "%s/.write-probe", path);
    FILE *fp = fopen(probe, "wb");
    if (!fp) return false;
    fputc('1', fp);
    fclose(fp);
    unlink(probe);
    return true;
}

void qzcam_storage_refresh(void)
{
    dir[0] = '\0';
    ready = false;

    const char *forced = getenv("QZCAM_DIR");
    if (forced && forced[0]) {
        if (mkdir(forced, 0755) == 0 || writable(forced)) {
            snprintf(dir, sizeof(dir), "%s", forced);
            snprintf(label, sizeof(label), "指定目录");
            ready = true;
            return;
        }
    }

    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        struct stat st;
        if (stat(candidates[i].dir, &st) != 0) {
            /* 不存在就试着建（/mnt/sdcard 是挂载点，建了也没用，所以只对后两个建） */
            if (i == 0) continue;
            if (mkdir(candidates[i].dir, 0755) != 0) continue;
        }
        if (!writable(candidates[i].dir)) continue;
        snprintf(dir, sizeof(dir), "%s", candidates[i].dir);
        snprintf(label, sizeof(label), "%s", candidates[i].label);
        ready = true;
        return;
    }
    snprintf(label, sizeof(label), "不可写");
}

/* 周期性重扫：开机没插卡先落到 flash，之后插上 SD 卡，新照片/录像自动
 * 切到 SD（已写的内容不动）。重扫很便宜（一次 stat + 一次写探针），UI 每
 * 秒都会进来，32 次约半分钟重探一次足够灵敏。 */
static unsigned probe_tick;

static void refresh_if_stale(void)
{
    if (dir[0] == '\0' || ++probe_tick % 32 == 0)
        qzcam_storage_refresh();
}

const char *qzcam_storage_dir(void)
{
    refresh_if_stale();
    return dir[0] ? dir : "/tmp";
}

const char *qzcam_storage_label(void)
{
    refresh_if_stale();
    return label;
}

bool qzcam_storage_ready(void)
{
    refresh_if_stale();
    return ready;
}

unsigned long long qzcam_storage_free_bytes(void)
{
    struct statvfs vfs;
    if (statvfs(qzcam_storage_dir(), &vfs) != 0) return 0;
    return (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
}

int qzcam_storage_free_percent(void)
{
    struct statvfs vfs;
    if (statvfs(qzcam_storage_dir(), &vfs) != 0) return 100;
    unsigned long long total = (unsigned long long)vfs.f_blocks;
    if (total == 0) return 100;
    return (int)((unsigned long long)vfs.f_bavail * 100ULL / total);
}
