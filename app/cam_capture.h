#ifndef QZCAM_CAPTURE_H
#define QZCAM_CAPTURE_H
#include <stdbool.h>
#include <stdint.h>
bool qzcam_capture_init(void);
void qzcam_capture_close(void);
bool qzcam_capture_request(int delay_seconds, int count);
void qzcam_capture_cancel(void);
bool qzcam_capture_busy(void);
int qzcam_capture_countdown(void);
/* Main/UI thread: collects completion messages and handles motion detection. */
bool qzcam_capture_message(char *out, unsigned size);
void qzcam_capture_motion(const uint16_t *pixels, int w, int h);
#endif
