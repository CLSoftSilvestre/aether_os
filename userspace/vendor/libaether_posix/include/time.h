#ifndef _POSIX_TIME_H
#define _POSIX_TIME_H

#include <sys/types.h>

typedef long time_t;
typedef long clockid_t;

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct tm {
    int tm_sec;    /* seconds [0,60] */
    int tm_min;    /* minutes [0,59] */
    int tm_hour;   /* hour [0,23] */
    int tm_mday;   /* day of month [1,31] */
    int tm_mon;    /* months since January [0,11] */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* days since Sunday [0,6] */
    int tm_yday;   /* days since January 1 [0,365] */
    int tm_isdst;  /* Daylight Saving Time flag */
};

time_t     time(time_t *t);
int        clock_gettime(clockid_t clk, struct timespec *tp);
int        gettimeofday(struct timeval *tv, struct timezone *tz);
struct tm *gmtime(const time_t *timep);
struct tm *localtime(const time_t *timep);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
struct tm *localtime_r(const time_t *timep, struct tm *result);
time_t     mktime(struct tm *tm);
double     difftime(time_t t1, time_t t0);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

#endif /* _POSIX_TIME_H */
