#define _POSIX_C_SOURCE 200809L
#include "cam_media.h"
#include "cam_storage.h"
#include "cam_record.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool suffix(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcmp(name + n - e, ext);
}
bool qzcam_media_name(const char *name)
{
    if (!name || !name[0] || strlen(name) >= 160 || strchr(name, '/') || strstr(name, "..")) return false;
    if (strstr(name, ".thumb.") || strstr(name, ".original.")) return false;
    return suffix(name, ".jpg") || suffix(name, ".jpeg") || suffix(name, ".bmp") ||
           suffix(name, ".avi") || suffix(name, ".mp4");
}
bool qzcam_media_path(const char *name, char *out, size_t capacity)
{
    if (!qzcam_media_name(name)) return false;
    int n = snprintf(out, capacity, "%s/%s", qzcam_storage_dir(), name);
    return n > 0 && (size_t)n < capacity;
}
bool qzcam_media_locked(const char *name)
{
    char path[512], side[528];
    if (!qzcam_media_path(name, path, sizeof(path))) return false;
    snprintf(side, sizeof(side), "%s.lock", path);
    return access(side, F_OK) == 0;
}
bool qzcam_media_lock(const char *name, bool locked)
{
    char path[512], side[528];
    struct stat st;
    if (!qzcam_media_path(name, path, sizeof(path)) || stat(path, &st) || !S_ISREG(st.st_mode)) return false;
    snprintf(side, sizeof(side), "%s.lock", path);
    if (!locked) return unlink(side) == 0 || errno == ENOENT;
    FILE *f = fopen(side, "wb");
    if (!f) return false;
    return fclose(f) == 0;
}
static int newest(const void *a, const void *b)
{
    const qzcam_media_entry_t *x = a, *y = b;
    if (x->modified != y->modified) return x->modified < y->modified ? 1 : -1;
    return strcmp(y->name, x->name);
}
size_t qzcam_media_list(qzcam_media_entry_t *entries, size_t capacity, size_t offset)
{
    DIR *dir = opendir(qzcam_storage_dir());
    if (!dir || !entries || !capacity) { if (dir) closedir(dir); return 0; }
    qzcam_media_entry_t *all = NULL;
    size_t count = 0, allocated = 0;
    struct dirent *item;
    while ((item = readdir(dir))) {
        if (!qzcam_media_name(item->d_name)) continue;
        char path[512]; struct stat st;
        if (!qzcam_media_path(item->d_name, path, sizeof(path)) || lstat(path, &st) || !S_ISREG(st.st_mode)) continue;
        if (count == allocated) {
            size_t next = allocated ? allocated * 2 : 64;
            if (next > 65536) break;
            qzcam_media_entry_t *p = realloc(all, next * sizeof(*all));
            if (!p) break;
            all = p; allocated = next;
        }
        qzcam_media_entry_t *e = &all[count++];
        snprintf(e->name, sizeof(e->name), "%s", item->d_name);
        e->size = (unsigned long long)st.st_size; e->modified = st.st_mtime;
        e->video = suffix(e->name, ".avi") || suffix(e->name, ".mp4");
        e->locked = qzcam_media_locked(e->name);
    }
    closedir(dir);
    if (count) qsort(all, count, sizeof(*all), newest);
    size_t n = offset < count ? count - offset : 0;
    if (n > capacity) n = capacity;
    if (n) memcpy(entries, all + offset, n * sizeof(*all));
    free(all);
    return n;
}
bool qzcam_media_delete(const char *name)
{
    char path[512], side[540];
    if (!qzcam_media_path(name, path, sizeof(path)) || qzcam_media_locked(name)) return false;
    if (qzcam_rec_active()) return false; /* Recording owns the storage until finalized. */
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || unlink(path)) return false;
    const char *exts[] = {".thumb.jpg", ".original.jpg", ".lock"};
    for (int i = 0; i < 3; i++) { snprintf(side, sizeof(side), "%s%s", path, exts[i]); unlink(side); }
    return true;
}
bool qzcam_media_prune(unsigned long long reserve)
{
    if (qzcam_storage_free_bytes() >= reserve) return true;
    /* Only finalized camera recordings are candidates for automatic overwrite. */
    qzcam_media_entry_t entries[64];
    char oldest[160] = "";
    time_t oldest_time = (time_t)0x7fffffff;
    for (size_t offset = 0;; offset += 64) {
        size_t n = qzcam_media_list(entries, 64, offset);
        for (size_t i = 0; i < n; i++) {
            if (entries[i].video && !entries[i].locked && !strncmp(entries[i].name, "VID_", 4) &&
                entries[i].modified <= oldest_time) {
                snprintf(oldest, sizeof(oldest), "%s", entries[i].name);
                oldest_time = entries[i].modified;
            }
        }
        if (n < 64) break;
    }
    if (!oldest[0]) return false;
    char path[512], side[540];
    if (!qzcam_media_path(oldest, path, sizeof(path)) || unlink(path)) return false;
    snprintf(side, sizeof(side), "%s.thumb.jpg", path); unlink(side);
    return qzcam_storage_free_bytes() >= reserve || qzcam_media_prune(reserve);
}
bool qzcam_media_thumbnail(const char *name, char *out, size_t capacity)
{
    char path[512];
    if (!qzcam_media_path(name, path, sizeof(path))) return false;
    int n = snprintf(out, capacity, "%s.thumb.jpg", path);
    return n > 0 && (size_t)n < capacity && access(out, R_OK) == 0;
}

/* Recover incomplete AVI files by retaining complete chunks and rebuilding idx1. */
static unsigned le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static bool patch32(FILE *f, long pos, unsigned v)
{
    uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24};
    return !fseek(f, pos, SEEK_SET) && fwrite(b, 1, 4, f) == 4;
}
static bool recover_avi(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    uint8_t header[256];
    if (fread(header, 1, 256, f) != 256 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "AVI ", 4)) { fclose(f); return false; }
    long movi = 0, avih = 0, strh = 0;
    for (int i = 12; i <= 244; i++) {
        if (!memcmp(header + i, "movi", 4)) movi = i + 4;
        if (!memcmp(header + i, "avih", 4)) avih = i + 24;
        if (!memcmp(header + i, "strh", 4)) strh = i + 40;
    }
    if (!movi || !avih || !strh) { fclose(f); return false; }
    fseek(f, 0, SEEK_END); long length = ftell(f);
    long pos = movi;
    unsigned count = 0;
    uint32_t *offsets = NULL, *sizes = NULL;
    while (pos + 8 <= length && count < 20000) {
        uint8_t b[8]; fseek(f, pos, SEEK_SET);
        if (fread(b, 1, 8, f) != 8 || memcmp(b, "00dc", 4)) break;
        unsigned sz = le32(b + 4);
        if (!sz || sz > 32000000 || pos + 8 + sz > length) break;
        uint32_t *next_o = realloc(offsets, (count + 1) * sizeof(*offsets));
        if (!next_o) break;
        offsets = next_o;
        uint32_t *next_s = realloc(sizes, (count + 1) * sizeof(*sizes));
        if (!next_s) break;
        sizes = next_s;
        offsets[count] = (uint32_t)(pos - movi); sizes[count++] = sz;
        pos += 8 + ((sz + 1) & ~1u);
    }
    bool ok = count > 0 && !ftruncate(fileno(f), pos) && !fseek(f, pos, SEEK_SET);
    uint8_t tag[8] = {'i','d','x','1',0,0,0,0};
    unsigned idx = count * 16; for (int i = 0; i < 4; i++) tag[i + 4] = (uint8_t)(idx >> (i * 8));
    if (ok) ok = fwrite(tag, 1, 8, f) == 8;
    for (unsigned i = 0; ok && i < count; i++) {
        uint8_t e[16] = {'0','0','d','c',16,0,0,0};
        for (int k = 0; k < 4; k++) { e[k + 8] = offsets[i] >> (8 * k); e[k + 12] = sizes[i] >> (8 * k); }
        ok = fwrite(e, 1, 16, f) == 16;
    }
    long end = ftell(f);
    if (ok) ok = patch32(f, 4, (unsigned)(end - 8)) && patch32(f, movi - 8, (unsigned)(pos - movi + 4)) &&
                 patch32(f, avih, count) && patch32(f, strh, count) && !fflush(f) && !fsync(fileno(f));
    free(offsets); free(sizes); fclose(f);
    return ok;
}
int qzcam_media_recover(void)
{
    DIR *dir = opendir(qzcam_storage_dir());
    if (!dir) return 0;
    int recovered = 0; struct dirent *e;
    while ((e = readdir(dir))) {
        if (!suffix(e->d_name, ".avi.part")) continue;
        char path[512], final[512];
        snprintf(path, sizeof(path), "%s/%s", qzcam_storage_dir(), e->d_name);
        snprintf(final, sizeof(final), "%s", path); final[strlen(final) - 5] = 0;
        if (access(final, F_OK) == 0) continue;
        if (recover_avi(path) && !rename(path, final)) recovered++;
    }
    closedir(dir); return recovered;
}
