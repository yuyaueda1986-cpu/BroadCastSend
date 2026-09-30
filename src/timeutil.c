#define _GNU_SOURCE
#include "timeutil.h"

#include <errno.h>
#include <stdio.h>

void format_utc_now(char *buf, size_t len)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000);
}

const char *errno_name(int e)
{
    switch (e) {
    case EAGAIN: return "EAGAIN";
    case ENOBUFS: return "ENOBUFS";
    case EINTR: return "EINTR";
    case EMSGSIZE: return "EMSGSIZE";
    case ENETUNREACH: return "ENETUNREACH";
    case EHOSTUNREACH: return "EHOSTUNREACH";
    case ENETDOWN: return "ENETDOWN";
    case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case EINVAL: return "EINVAL";
    case EBADF: return "EBADF";
    case ENOMEM: return "ENOMEM";
    case ENODEV: return "ENODEV";
    case ENXIO: return "ENXIO";
    default: return "?";
    }
}
