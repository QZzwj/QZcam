#ifndef QZCAM_MEDIA_H
#define QZCAM_MEDIA_H
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
typedef struct {
    char name[160];
    unsigned long long size;
    time_t modified;
    bool video;
    bool locked;
} qzcam_media_entry_t;
bool qzcam_media_name(const char *name);
size_t qzcam_media_list(qzcam_media_entry_t *entries, size_t capacity, size_t offset);
bool qzcam_media_path(const char *name, char *out, size_t capacity);
bool qzcam_media_delete(const char *name);
bool qzcam_media_lock(const char *name, bool locked);
bool qzcam_media_locked(const char *name);
bool qzcam_media_prune(unsigned long long reserve);
bool qzcam_media_thumbnail(const char *name, char *out, size_t capacity);
int qzcam_media_recover(void);
#endif
