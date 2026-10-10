#define _GNU_SOURCE
#include "cam_device.h"
#include "cam_capture.h"
#include "qzcam.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

static unsigned long long activity;
static int applied = -1;
static bool sleeping;
static time_t clock_offset;
static unsigned long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void qzcam_device_activity(void) { activity = now_ms(); sleeping = false; }
bool qzcam_device_sleeping(void) { return sleeping; }
static void brightness(int percent)
{
    const char *forced = getenv("QZCAM_BACKLIGHT");
    char base[256] = "";
    if (forced) snprintf(base, sizeof(base), "%s", forced);
    else {
        DIR *d = opendir("/sys/class/backlight"); struct dirent *e;
        if (!d) return;
        while ((e = readdir(d))) if (e->d_name[0] != '.') { snprintf(base, sizeof(base), "/sys/class/backlight/%.180s", e->d_name); break; }
        closedir(d);
    }
    if (!base[0]) return;
    char path[288]; int max = 255;
    snprintf(path, sizeof(path), "%s/max_brightness", base);
    FILE *f = fopen(path, "r"); if (f) { if (fscanf(f, "%d", &max) != 1) max = 255; fclose(f); }
    snprintf(path, sizeof(path), "%s/brightness", base);
    f = fopen(path, "w"); if (f) { fprintf(f, "%d\n", max * percent / 100); fclose(f); }
}
void qzcam_device_poll(bool recording)
{
    const qzcam_settings_t *s = qzcam_settings();
    if (!activity) activity = now_ms();
    unsigned long long idle = now_ms() - activity;
    sleeping = s->screen_timeout > 0 && idle >= (unsigned long long)s->screen_timeout * 1000;
    int level = sleeping ? 0 : s->screen_brightness;
    if (level != applied) { brightness(level); applied = level; }
#if !defined(QZDESK_SIMULATOR)
    if (s->auto_off > 0 && !recording && !qzcam_capture_busy() && idle >= (unsigned long long)s->auto_off * 1000) {
        qzcam_settings_save(); sync(); reboot(RB_POWER_OFF);
        activity = now_ms();
    }
#else
    (void)recording;
#endif
}
time_t qzcam_device_time(void) { return time(NULL) + clock_offset; }
bool qzcam_device_set_time(const char *iso)
{
    struct tm parts = {0};
    int y, m, d, hour, min, sec = 0; char extra;
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d%c", &y, &m, &d, &hour, &min, &sec, &extra) != 6 ||
        y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31 || hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) return false;
    parts.tm_year = y - 1900; parts.tm_mon = m - 1; parts.tm_mday = d;
    parts.tm_hour = hour; parts.tm_min = min; parts.tm_sec = sec; parts.tm_isdst = -1;
    time_t target = mktime(&parts);
    if (parts.tm_mday != d || parts.tm_mon != m - 1 || target == (time_t)-1) return false;
#if defined(QZDESK_SIMULATOR)
    clock_offset = target - time(NULL); return true;
#else
    struct timespec t = {target, 0}; return clock_settime(CLOCK_REALTIME, &t) == 0;
#endif
}
