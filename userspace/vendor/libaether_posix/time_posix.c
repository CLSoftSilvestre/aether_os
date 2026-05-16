/*
 * libaether_posix/time_posix.c — POSIX time functions
 *
 * Time source: AetherOS RTC (PL031) via sys_rtc_get() for wall-clock seconds.
 * High-resolution: CNTPCT_EL0 is already read in boot_prof.c at 54 MHz.
 * Here we use SYS_GET_TICKS (100 Hz tick counter) for sub-second resolution
 * as a conservative fallback.
 *
 * Full CNTFRQ / CNTPCT access from userspace can be added via a dedicated
 * syscall (SYS_CNTPCT_GET) in a future iteration.
 */

#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <sys.h>   /* AetherOS syscalls */

/* ── time() ──────────────────────────────────────────────────────────── */

time_t time(time_t *t)
{
    time_t now = (time_t)sys_rtc_get();
    if (t) *t = now;
    return now;
}

/* ── clock_gettime() ─────────────────────────────────────────────────── */

int clock_gettime(clockid_t clk, struct timespec *tp)
{
    if (!tp) { errno = EFAULT; return -1; }

    time_t sec = (time_t)sys_rtc_get();
    long   ticks = (long)sys_get_ticks(); /* 100 Hz */

    switch (clk) {
    case CLOCK_REALTIME:
        tp->tv_sec  = sec;
        tp->tv_nsec = (ticks % 100L) * 10000000L; /* ticks → ns at 100 Hz */
        return 0;

    case CLOCK_MONOTONIC:
        /* Monotonic: seconds since boot approximated from tick counter */
        tp->tv_sec  = ticks / 100L;
        tp->tv_nsec = (ticks % 100L) * 10000000L;
        return 0;

    default:
        errno = EINVAL;
        return -1;
    }
}

/* ── gettimeofday() ──────────────────────────────────────────────────── */

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    (void)tz;
    if (!tv) { errno = EFAULT; return -1; }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tv->tv_sec  = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000L;
    return 0;
}

/* ── Calendar math ─────────────────────────────────────────────────────
 *
 * Minimal Gregorian calendar — no leap-second, no DST, UTC only.
 * Algorithms from Howard Hinnant's date library (public domain).
 */

static int _is_leap(int y) { return (y%4==0 && y%100!=0) || y%400==0; }

static int _yday_from_ym(int y, int m)   /* 0-based month, returns 0-based yday */
{
    static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int d = 0;
    for (int i = 0; i < m; i++) d += md[i];
    if (m >= 2 && _is_leap(y)) d++;
    return d;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
    if (!timep || !result) { errno = EFAULT; return 0; }

    long t = (long)*timep;
    if (t < 0) t = 0; /* clamp to epoch */

    long days   = t / 86400L;
    long secs   = t % 86400L;

    result->tm_hour = (int)(secs / 3600);
    result->tm_min  = (int)((secs % 3600) / 60);
    result->tm_sec  = (int)(secs % 60);
    result->tm_wday = (int)((days + 4) % 7); /* 1970-01-01 = Thursday = 4 */

    /* Days → year/month/mday */
    long z   = days + 719468L;
    long era = (z >= 0 ? z : z - 146096L) / 146097L;
    long doe = z - era * 146097L;
    long yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long y   = yoe + era * 400L;
    long doy = doe - (365*yoe + yoe/4 - yoe/100);
    long mp  = (5*doy + 2) / 153;
    long d   = doy - (153*mp + 2)/5 + 1;
    long m   = mp < 10 ? mp + 3 : mp - 9;
    y       += m <= 2;

    result->tm_year  = (int)(y - 1900);
    result->tm_mon   = (int)(m - 1);
    result->tm_mday  = (int)d;
    result->tm_yday  = _yday_from_ym((int)y, (int)(m-1)) + (int)d - 1;
    result->tm_isdst = 0;

    return result;
}

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
    return gmtime_r(timep, result); /* no timezone support */
}

/* ── mktime() ─────────────────────────────────────────────────────────── */

time_t mktime(struct tm *tm)
{
    /* Days from epoch to year */
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon;   /* 0-11 */
    int d = tm->tm_mday;  /* 1-31 */

    /* Shift months so March is 0 (simplifies leap-year logic) */
    if (m < 2) { m += 12; y--; }
    long days = (long)(365*y) + y/4 - y/100 + y/400
                + (153*((long)m - 2) + 2)/5 + (long)d - 719469L;

    time_t t = (time_t)(days * 86400L)
             + (time_t)(tm->tm_hour * 3600)
             + (time_t)(tm->tm_min  * 60)
             + (time_t) tm->tm_sec;

    /* Fill in derived fields */
    struct tm tmp;
    gmtime_r(&t, &tmp);
    *tm = tmp;

    return t;
}

double difftime(time_t t1, time_t t0)
{
    return (double)(t1 - t0);
}

/* ── ctime() — non-reentrant time_t → "Www Mmm Dd HH:MM:SS YYYY\n" ─────── */

char *ctime(const time_t *timep)
{
    static char buf[32];
    struct tm *tm = gmtime(timep);
    strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y\n", tm);
    return buf;
}

/* ── gmtime() / localtime() — non-reentrant wrappers ─────────────────── */

static struct tm _gmtime_buf;

struct tm *gmtime(const time_t *timep)
{
    return gmtime_r(timep, &_gmtime_buf);
}

struct tm *localtime(const time_t *timep)
{
    return gmtime_r(timep, &_gmtime_buf);
}

/* ── strftime() — minimal subset used by libpng / libjpeg ─────────────── */

#include <string.h>

static void _itoa2(char *buf, int v)
{
    buf[0] = '0' + (v / 10) % 10;
    buf[1] = '0' + v % 10;
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
    static const char *days[]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[]= {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    size_t n = 0;
    char tmp[32];

#define EMIT(str) do { \
    const char *_s = (str); \
    while (*_s && n + 1 < max) s[n++] = *_s++; \
} while(0)

    while (*fmt && n + 1 < max) {
        if (*fmt != '%') { s[n++] = *fmt++; continue; }
        fmt++;
        switch (*fmt++) {
        case 'Y': { int y = tm->tm_year+1900;
                    tmp[0]='0'+y/1000; tmp[1]='0'+(y/100)%10;
                    tmp[2]='0'+(y/10)%10; tmp[3]='0'+y%10; tmp[4]=0;
                    EMIT(tmp); break; }
        case 'y': _itoa2(tmp, (tm->tm_year+1900)%100); tmp[2]=0; EMIT(tmp); break;
        case 'm': _itoa2(tmp, tm->tm_mon+1);  tmp[2]=0; EMIT(tmp); break;
        case 'd': _itoa2(tmp, tm->tm_mday);   tmp[2]=0; EMIT(tmp); break;
        case 'H': _itoa2(tmp, tm->tm_hour);   tmp[2]=0; EMIT(tmp); break;
        case 'M': _itoa2(tmp, tm->tm_min);    tmp[2]=0; EMIT(tmp); break;
        case 'S': _itoa2(tmp, tm->tm_sec);    tmp[2]=0; EMIT(tmp); break;
        case 'A': EMIT(days[tm->tm_wday % 7]); break;
        case 'a': { const char *d = days[tm->tm_wday%7];
                    tmp[0]=d[0]; tmp[1]=d[1]; tmp[2]=d[2]; tmp[3]=0; EMIT(tmp); break; }
        case 'B': EMIT(months[tm->tm_mon % 12]); break;
        case 'b': { const char *mn = months[tm->tm_mon%12];
                    tmp[0]=mn[0]; tmp[1]=mn[1]; tmp[2]=mn[2]; tmp[3]=0; EMIT(tmp); break; }
        case 'Z': EMIT("UTC"); break;
        case 'n': s[n++] = '\n'; break;
        case 't': s[n++] = '\t'; break;
        case '%': s[n++] = '%'; break;
        default:  s[n++] = '%'; if (n+1<max) s[n++] = fmt[-1]; break;
        }
    }
    s[n] = '\0';
    return n;

#undef EMIT
}
