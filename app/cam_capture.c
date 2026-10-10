#define _POSIX_C_SOURCE 200809L
#include "cam_capture.h"
#include "cam_core.h"
#include "cam_record.h"
#include "qzcam.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool initialized, exiting, busy, cancelled;
static int requested_count, delay;
static unsigned long long deadline;
static char message[320];
static unsigned long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void *capture_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!exiting) {
        while (!busy && !exiting) pthread_cond_wait(&ready, &lock);
        if (exiting) break;
        int count = requested_count;
        pthread_mutex_unlock(&lock);
        int saved = 0; bool failed = false;
        for (int i = 0; i < count; i++) {
            for (;;) {
                pthread_mutex_lock(&lock);
                bool stop = cancelled || exiting;
                bool due = now_ms() >= deadline;
                pthread_mutex_unlock(&lock);
                if (stop || due) break;
                struct timespec wait = {0, 50000000}; nanosleep(&wait, NULL);
            }
            pthread_mutex_lock(&lock);
            bool stop = cancelled || exiting;
            pthread_mutex_unlock(&lock);
            if (stop) break;
            char path[320];
            if (!qzcam_cam_snapshot(path, sizeof(path))) { failed = true; break; }
            saved++;
            pthread_mutex_lock(&lock);
            deadline = now_ms() + 250;
            pthread_mutex_unlock(&lock);
        }
        pthread_mutex_lock(&lock);
        if (failed) snprintf(message, sizeof(message), "拍照失败：%s", qzcam_cam_status());
        else snprintf(message, sizeof(message), cancelled ? "已取消，保存 %d 张" : "已保存 %d 张高清照片", saved);
        busy = false;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
bool qzcam_capture_init(void)
{
    if (initialized) return true;
    exiting = false;
    initialized = pthread_create(&thread, NULL, capture_worker, NULL) == 0;
    return initialized;
}
void qzcam_capture_close(void)
{
    if (!initialized) return;
    pthread_mutex_lock(&lock); exiting = true; cancelled = true;
    pthread_cond_signal(&ready); pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL); initialized = false;
}
bool qzcam_capture_request(int delay_seconds, int count)
{
    if (!initialized && !qzcam_capture_init()) return false;
    if (!qzcam_cam_healthy() || count < 1 || count > 10 || delay_seconds < 0 || delay_seconds > 10) return false;
    pthread_mutex_lock(&lock);
    if (busy) { pthread_mutex_unlock(&lock); return false; }
    delay = delay_seconds;
    deadline = now_ms() + (unsigned long long)delay * 1000;
    requested_count = count; cancelled = false; busy = true;
    pthread_cond_signal(&ready); pthread_mutex_unlock(&lock);
    return true;
}
void qzcam_capture_cancel(void) { pthread_mutex_lock(&lock); cancelled = true; pthread_mutex_unlock(&lock); }
bool qzcam_capture_busy(void) { pthread_mutex_lock(&lock); bool b = busy; pthread_mutex_unlock(&lock); return b; }
int qzcam_capture_countdown(void)
{
    pthread_mutex_lock(&lock);
    unsigned long long now = now_ms();
    int n = busy && deadline > now ? (int)((deadline - now + 999) / 1000) : 0;
    pthread_mutex_unlock(&lock); return n;
}
bool qzcam_capture_message(char *out, unsigned size)
{
    pthread_mutex_lock(&lock); bool has = message[0] != 0;
    if (has) { snprintf(out, size, "%s", message); message[0] = 0; }
    pthread_mutex_unlock(&lock); return has;
}
void qzcam_capture_motion(const uint16_t *pixels, int w, int h)
{
    enum { W = 32, H = 24 };
    static uint8_t previous[W * H];
    static bool warmed, motion_recording;
    static unsigned long long last_sample, last_motion;
    static int consecutive;
    const qzcam_settings_t *s = qzcam_settings();
    unsigned long long now = now_ms();
    if (!s->motion_detect) {
        warmed = false; consecutive = 0;
        if (motion_recording) { qzcam_rec_stop(); motion_recording = false; }
        return;
    }
    if (!pixels || now - last_sample < 200 || !qzcam_cam_healthy()) return;
    last_sample = now;
    int delta[W * H], average = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t p = pixels[(size_t)(y * h / H) * w + x * w / W];
            int luma = ((((p >> 11) & 31) * 77 * 8) + (((p >> 5) & 63) * 150 * 4) + ((p & 31) * 29 * 8)) >> 8;
            int i = y * W + x;
            delta[i] = luma - previous[i]; average += delta[i]; previous[i] = (uint8_t)luma;
        }
    }
    if (!warmed) { warmed = true; return; }
    average /= W * H;
    int changed = 0, threshold = 26 - s->motion_sensitivity / 5;
    for (int i = 0; i < W * H; i++) if (abs(delta[i] - average) > threshold) changed++;
    if (changed > W * H * (15 - s->motion_sensitivity / 10) / 100) consecutive++;
    else consecutive = 0;
    if (consecutive >= 2) {
        last_motion = now;
        if (s->mode == 1) {
            static unsigned long long last_photo;
            if (now - last_photo > 3000 && qzcam_capture_request(0, 1)) last_photo = now;
        } else if (!qzcam_rec_active()) {
            char path[320]; motion_recording = qzcam_rec_start(path, sizeof(path));
        }
    }
    if (motion_recording && (!qzcam_rec_active() || now - last_motion > (unsigned long long)s->motion_hold * 1000)) {
        qzcam_rec_stop(); motion_recording = false;
    }
}
