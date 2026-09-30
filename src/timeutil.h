#ifndef BCS_TIMEUTIL_H
#define BCS_TIMEUTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

static inline int64_t mono_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline struct timespec ns_to_timespec(int64_t ns)
{
    struct timespec ts = { .tv_sec = (time_t)(ns / 1000000000LL), .tv_nsec = (long)(ns % 1000000000LL) };
    return ts;
}

/* 現在のUTC時刻をISO 8601形式（マイクロ秒）で書き込む */
void format_utc_now(char *buf, size_t len);

/* errnoの記号名（EAGAINなど）。不明なら"?" */
const char *errno_name(int e);

#endif
