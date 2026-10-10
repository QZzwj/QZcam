#ifndef QZCAM_DEVICE_H
#define QZCAM_DEVICE_H
#include <stdbool.h>
#include <time.h>
void qzcam_device_activity(void);
bool qzcam_device_sleeping(void);
void qzcam_device_poll(bool recording);
bool qzcam_device_set_time(const char *iso);
time_t qzcam_device_time(void);
#endif
