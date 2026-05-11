/*
 * libaether_posix/stdio_posix.c — POSIX stdio over AetherOS VFS syscalls
 *
 * FILE* implementation details:
 *   fd < 0     : closed / invalid
 *   fd = 0/1/2 : stdin/stdout/stderr — use sys_read/sys_write
 *   fd ≥ 100   : network socket — use sys_net_send/sys_net_recv
 *   fd ≥ 200   : VFS file — use sys_fs_read/sys_fs_write/sys_fs_close
 *
 * Buffering strategy:
 *   - Read: 256-byte read-ahead buffer (reduces VFS call count)
 *   - Write: unbuffered on VFS; line-buffered on stdout/stderr (flush on \n)
 *   - fflush() is always a no-op since writes go straight through
 *
 * fseek on VFS files: AetherOS VFS has no seek syscall yet (Phase 7.6 adds
 * SYS_FS_SEEK).  For now we track position in FILE.pos and implement:
 *   SEEK_SET forward: read-and-discard from current pos
 *   SEEK_SET backward: re-open the file (requires stored path)
 *   SEEK_CUR forward: read-and-discard
 *   SEEK_END: not supported (returns -1 / ESPIPE)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <sys.h>   /* AetherOS syscalls (from lib/include/) */

/* ── FILE struct ─────────────────────────────────────────────────────── */

#define FILE_FLAG_PIPE   0x01   /* fd 0/1/2 or socket — no VFS seek */
#define FILE_FLAG_SOCKET 0x02   /* fd ≥ 100 (network socket) */
#define FILE_FLAG_WRITE  0x04   /* opened for writing */
#define FILE_FLAG_READ   0x08   /* opened for reading */

#define RBUF_SZ 256
#define PATH_CACHE 256

struct _posix_file {
    int    fd;
    int    flags;
    int    error;
    int    eof;
    long   pos;           /* logical read position (bytes from start) */
    char   rbuf[RBUF_SZ];
    int    rbuf_off;
    int    rbuf_len;
    char   path[PATH_CACHE]; /* for backward seek re-open */
};

#define MAX_FILES 32
static struct _posix_file _file_pool[MAX_FILES];
static int _files_init = 0;

static void _init_files(void)
{
    for (int i = 0; i < MAX_FILES; i++) _file_pool[i].fd = -1;
    /* wire stdin/stdout/stderr */
    _file_pool[0].fd = 0; _file_pool[0].flags = FILE_FLAG_PIPE | FILE_FLAG_READ;
    _file_pool[1].fd = 1; _file_pool[1].flags = FILE_FLAG_PIPE | FILE_FLAG_WRITE;
    _file_pool[2].fd = 2; _file_pool[2].flags = FILE_FLAG_PIPE | FILE_FLAG_WRITE;
    _files_init = 1;
}

static struct _posix_file *_alloc_file(void)
{
    if (!_files_init) _init_files();
    for (int i = 3; i < MAX_FILES; i++)
        if (_file_pool[i].fd < 0) {
            memset(&_file_pool[i], 0, sizeof(_file_pool[i]));
            return &_file_pool[i];
        }
    return NULL;
}

/* Special singleton FILE objects for stdin/stdout/stderr */
FILE *stdin  = &_file_pool[0];
FILE *stdout = &_file_pool[1];
FILE *stderr = &_file_pool[2];

/* ── fopen ───────────────────────────────────────────────────────────── */

FILE *fopen(const char *path, const char *mode)
{
    if (!_files_init) _init_files();
    int write_mode = 0;
    for (const char *m = mode; *m; m++) {
        if (*m == 'w' || *m == 'a') write_mode = 1;
    }

    long fd;
    if (write_mode)
        fd = sys_fs_create(path);
    else
        fd = sys_fs_open(path);

    if (fd < 0) { errno = ENOENT; return NULL; }

    struct _posix_file *f = _alloc_file();
    if (!f) { sys_fs_close(fd); errno = EMFILE; return NULL; }

    f->fd       = (int)fd;
    f->flags    = write_mode ? FILE_FLAG_WRITE : FILE_FLAG_READ;
    f->error    = 0;
    f->eof      = 0;
    f->pos      = 0;
    f->rbuf_off = 0;
    f->rbuf_len = 0;

    size_t plen = strlen(path);
    if (plen >= PATH_CACHE) plen = PATH_CACHE - 1;
    memcpy(f->path, path, plen);
    f->path[plen] = '\0';

    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *fp)
{
    if (fp && fp->fd >= 200) sys_fs_close(fp->fd);
    if (!path) return NULL;
    FILE *nf = fopen(path, mode);
    if (!nf) return NULL;
    /* Copy into fp's slot if fp is a valid pool entry */
    if (fp && fp >= _file_pool && fp < _file_pool + MAX_FILES) {
        *fp = *nf;
        nf->fd = -1; /* release the alloc'd slot */
        return fp;
    }
    return nf;
}

/* ── fclose ──────────────────────────────────────────────────────────── */

int fclose(FILE *fp)
{
    if (!fp || fp->fd < 0) { errno = EBADF; return -1; }
    if (fp->fd >= 200)
        sys_fs_close(fp->fd);
    else if (fp->fd >= 100)
        sys_net_close(fp->fd);
    /* fd 0/1/2: never close */
    fp->fd = -1;
    return 0;
}

int fflush(FILE *fp) { (void)fp; return 0; }
void setbuf(FILE *fp, char *buf) { (void)fp; (void)buf; }
int setvbuf(FILE *fp, char *buf, int mode, size_t size)
{
    (void)fp; (void)buf; (void)mode; (void)size; return 0;
}

/* ── Low-level read/write helpers ────────────────────────────────────── */

static long _file_read(struct _posix_file *f, void *buf, long len)
{
    if (f->fd == 0)
        return sys_read(0, buf, len);
    if (f->fd >= 100 && f->fd < 200)
        return sys_net_recv(f->fd, buf, len);
    return sys_fs_read(f->fd, buf, len);
}

static long _file_write(struct _posix_file *f, const void *buf, long len)
{
    if (f->fd == 1 || f->fd == 2)
        return sys_write(f->fd, buf, len);
    if (f->fd >= 100 && f->fd < 200)
        return sys_net_send(f->fd, buf, len);
    return sys_fs_write(f->fd, buf, len);
}

/* ── fread ───────────────────────────────────────────────────────────── */

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    if (!fp || fp->fd < 0 || fp->eof) return 0;
    size_t total = size * nmemb;
    if (!total) return 0;
    unsigned char *out = ptr;
    size_t got = 0;

    while (got < total) {
        /* Drain read-ahead buffer first */
        if (fp->rbuf_len > 0) {
            size_t chunk = (size_t)fp->rbuf_len;
            if (chunk > total - got) chunk = total - got;
            memcpy(out + got, fp->rbuf + fp->rbuf_off, chunk);
            fp->rbuf_off += (int)chunk;
            fp->rbuf_len -= (int)chunk;
            fp->pos      += (long)chunk;
            got          += chunk;
            continue;
        }

        /* Refill buffer */
        long n = _file_read(fp, fp->rbuf, RBUF_SZ);
        if (n <= 0) {
            if (n < 0) fp->error = 1;
            else       fp->eof   = 1;
            break;
        }
        fp->rbuf_off = 0;
        fp->rbuf_len = (int)n;
    }
    return size ? got / size : 0;
}

/* ── fwrite ──────────────────────────────────────────────────────────── */

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
    if (!fp || fp->fd < 0) return 0;
    size_t total = size * nmemb;
    if (!total) return 0;
    long n = _file_write(fp, ptr, (long)total);
    if (n < 0) { fp->error = 1; return 0; }
    return size ? (size_t)n / size : 0;
}

/* ── fgetc / fputc ───────────────────────────────────────────────────── */

int fgetc(FILE *fp)
{
    unsigned char c;
    if (fread(&c, 1, 1, fp) != 1) return EOF;
    return (int)c;
}

int fputc(int c, FILE *fp)
{
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, fp) != 1) return EOF;
    return c;
}

int getchar(void) { return fgetc(stdin); }
int putchar(int c) { return fputc(c, stdout); }

int ungetc(int c, FILE *fp)
{
    if (!fp || fp->fd < 0 || c == EOF) return EOF;
    if (fp->rbuf_off > 0) {
        fp->rbuf_off--;
        fp->rbuf_len++;
        fp->rbuf[fp->rbuf_off] = (char)c;
        if (fp->pos > 0) fp->pos--;
        fp->eof = 0;
        return c;
    }
    return EOF; /* buffer full — can't push back */
}

/* ── fgets / fputs ───────────────────────────────────────────────────── */

char *fgets(char *s, int n, FILE *fp)
{
    if (!s || n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(fp);
        if (c == EOF) { if (i == 0) return NULL; break; }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *fp)
{
    size_t len = strlen(s);
    return (int)fwrite(s, 1, len, fp);
}

int puts(const char *s)
{
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

/* ── Positioning ─────────────────────────────────────────────────────── */

long ftell(FILE *fp)
{
    if (!fp || fp->fd < 0) { errno = EBADF; return -1L; }
    return fp->pos - fp->rbuf_len; /* account for buffered but not consumed data */
}

int fseek(FILE *fp, long offset, int whence)
{
    if (!fp || fp->fd < 0) { errno = EBADF; return -1; }
    if (fp->flags & FILE_FLAG_PIPE) { errno = ESPIPE; return -1; }

    long target;
    long cur = ftell(fp);
    switch (whence) {
    case 0 /* SEEK_SET */: target = offset; break;
    case 1 /* SEEK_CUR */: target = cur + offset; break;
    default: errno = EINVAL; return -1;
    }

    if (target < 0) { errno = EINVAL; return -1; }

    if (target == cur) return 0;

    if (target > cur) {
        /* Forward seek: read-and-discard */
        long skip = target - cur;
        char tmp[64];
        while (skip > 0) {
            long chunk = skip < 64 ? skip : 64;
            long n = (long)fread(tmp, 1, (size_t)chunk, fp);
            if (n <= 0) break;
            skip -= n;
        }
        return 0;
    }

    /* Backward seek: re-open file and skip forward */
    if (!fp->path[0]) { errno = ESPIPE; return -1; }

    if (fp->fd >= 200) sys_fs_close(fp->fd);
    long fd = sys_fs_open(fp->path);
    if (fd < 0) { errno = EIO; return -1; }

    fp->fd      = (int)fd;
    fp->pos     = 0;
    fp->rbuf_off = 0;
    fp->rbuf_len = 0;
    fp->eof     = 0;
    fp->error   = 0;

    /* Skip to target */
    char tmp[64];
    long skip = target;
    while (skip > 0) {
        long chunk = skip < 64 ? skip : 64;
        long n = (long)fread(tmp, 1, (size_t)chunk, fp);
        if (n <= 0) break;
        skip -= n;
    }
    return 0;
}

void rewind(FILE *fp) { fseek(fp, 0, 0 /*SEEK_SET*/); }

int fgetpos(FILE *fp, long *pos) { *pos = ftell(fp); return 0; }
int fsetpos(FILE *fp, const long *pos) { return fseek(fp, *pos, 0); }

/* ── Status ──────────────────────────────────────────────────────────── */

int feof(FILE *fp)   { return fp ? fp->eof   : 1; }
int ferror(FILE *fp) { return fp ? fp->error : 1; }
void clearerr(FILE *fp) { if (fp) { fp->eof = 0; fp->error = 0; } }

/* ── vsnprintf ─────────────────────────────────────────────────────── */

static void _emit(char *buf, size_t sz, size_t *pos, char c)
{
    if (*pos + 1 < sz) buf[(*pos)++] = c;
}

static void _emit_str(char *buf, size_t sz, size_t *pos, const char *s,
                       int width, int left, int prec)
{
    if (!s) s = "(null)";
    size_t len = strlen(s);
    if (prec >= 0 && (size_t)prec < len) len = (size_t)prec;
    int pad = (width > (int)len) ? width - (int)len : 0;
    if (!left) while (pad-- > 0) _emit(buf, sz, pos, ' ');
    for (size_t i = 0; i < len; i++) _emit(buf, sz, pos, s[i]);
    if (left)  while (pad-- > 0) _emit(buf, sz, pos, ' ');
}

static void _emit_u64(char *buf, size_t sz, size_t *pos,
                       unsigned long long v, int base, int upper,
                       int width, char pad_ch, int alt, int is_signed, int neg)
{
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = upper ? hi : lo;
    char tmp[24]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; } }
    /* Sign / prefix */
    char pfx[3]; int pfx_len = 0;
    if (neg)              pfx[pfx_len++] = '-';
    else if (is_signed)   {} /* no prefix for positive */
    if (alt && base == 16) { pfx[pfx_len++] = '0'; pfx[pfx_len++] = upper?'X':'x'; }
    else if (alt && base == 8 && (n == 0 || tmp[n-1] != '0')) pfx[pfx_len++] = '0';

    int total = n + pfx_len;
    int padding = (width > total) ? width - total : 0;
    if (pad_ch == '0') {
        for (int i = 0; i < pfx_len; i++) _emit(buf, sz, pos, pfx[i]);
        while (padding-- > 0) _emit(buf, sz, pos, '0');
    } else {
        while (padding-- > 0) _emit(buf, sz, pos, ' ');
        for (int i = 0; i < pfx_len; i++) _emit(buf, sz, pos, pfx[i]);
    }
    while (n--) _emit(buf, sz, pos, tmp[n]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { _emit(buf, size, &pos, *p); continue; }
        p++;
        if (!*p) break;

        int left = 0, alt = 0, plus = 0, space = 0;
        char pad_ch = ' ';
        while (*p == '-'||*p == '0'||*p == '#'||*p == '+'||*p == ' ') {
            if (*p == '-') left = 1;
            if (*p == '0' && !left) pad_ch = '0';
            if (*p == '#') alt = 1;
            if (*p == '+') plus = 1;
            if (*p == ' ') space = 1;
            p++;
        }
        (void)plus; (void)space;

        int width = 0;
        if (*p == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } p++; }
        else while (*p >= '0' && *p <= '9') { width = width*10 + (*p - '0'); p++; }

        int prec = -1;
        if (*p == '.') {
            p++;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else { prec = 0; while (*p >= '0' && *p <= '9') { prec = prec*10 + (*p - '0'); p++; } }
        }

        int ll = 0, l = 0, h = 0, z = 0; int hh = 0; (void)hh;
        while (*p == 'l'||*p == 'h'||*p == 'z'||*p == 'j') {
            if (*p == 'l') { if (l) ll = 1; else l = 1; }
            if (*p == 'h') { if (h) hh = 1; else h = 1; }
            if (*p == 'z') z = 1;
            p++;
        }

        switch (*p) {
        case 'd': case 'i': {
            long long v = ll ? va_arg(ap, long long) :
                          l  ? (long long)va_arg(ap, long) :
                          z  ? (long long)va_arg(ap, long) :
                               (long long)va_arg(ap, int);
            int neg = v < 0;
            unsigned long long uv = neg ? (unsigned long long)(-v) : (unsigned long long)v;
            _emit_u64(buf, size, &pos, uv, 10, 0, width, pad_ch, 0, 1, neg);
            break; }
        case 'u': {
            unsigned long long v = ll ? va_arg(ap, unsigned long long) :
                                   l  ? (unsigned long long)va_arg(ap, unsigned long) :
                                   z  ? (unsigned long long)va_arg(ap, unsigned long) :
                                        (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 10, 0, width, pad_ch, 0, 0, 0);
            break; }
        case 'x': {
            unsigned long long v = ll ? va_arg(ap, unsigned long long) :
                                   l  ? (unsigned long long)va_arg(ap, unsigned long) :
                                        (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 16, 0, width, pad_ch, alt, 0, 0);
            break; }
        case 'X': {
            unsigned long long v = ll ? va_arg(ap, unsigned long long) :
                                   l  ? (unsigned long long)va_arg(ap, unsigned long) :
                                        (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 16, 1, width, pad_ch, alt, 0, 0);
            break; }
        case 'o': {
            unsigned long long v = ll ? va_arg(ap, unsigned long long) :
                                        (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 8, 0, width, pad_ch, alt, 0, 0);
            break; }
        case 'p': {
            unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            _emit(buf, size, &pos, '0'); _emit(buf, size, &pos, 'x');
            _emit_u64(buf, size, &pos, v, 16, 0, 0, ' ', 0, 0, 0);
            break; }
        case 's':
            _emit_str(buf, size, &pos, va_arg(ap, const char *), width, left, prec);
            break;
        case 'c':
            _emit(buf, size, &pos, (char)va_arg(ap, int));
            break;
        case '%':
            _emit(buf, size, &pos, '%');
            break;
        case 'n':
            *va_arg(ap, int *) = (int)pos;
            break;
        default:
            _emit(buf, size, &pos, '%');
            _emit(buf, size, &pos, *p);
            break;
        }
    }
    if (size > 0) buf[pos] = '\0';
    return (int)pos;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap); return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap); return r;
}

/* ── printf / fprintf ────────────────────────────────────────────────── */

int vfprintf(FILE *fp, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) fwrite(buf, 1, (size_t)n, fp);
    return n;
}

int fprintf(FILE *fp, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap); return r;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap); return r;
}

/* ── perror ──────────────────────────────────────────────────────────── */

void perror(const char *s)
{
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

/* ── sscanf (minimal) ────────────────────────────────────────────────── */

int sscanf(const char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int matched = 0;
    const char *p = buf;
    for (const char *f = fmt; *f && *p; f++) {
        if (*f != '%') { if (*p == *f) p++; else break; continue; }
        f++;
        int is_long = 0;
        if (*f == 'l') { is_long = 1; f++; }
        switch (*f) {
        case 'd': {
            while (isspace((unsigned char)*p)) p++;
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p) goto done;
            if (is_long) *va_arg(ap, long *) = v;
            else         *va_arg(ap, int *)  = (int)v;
            p = end; matched++;
            break; }
        case 'u': {
            while (isspace((unsigned char)*p)) p++;
            char *end;
            unsigned long v = strtoul(p, &end, 10);
            if (end == p) goto done;
            if (is_long) *va_arg(ap, unsigned long *) = v;
            else         *va_arg(ap, unsigned int *)  = (unsigned int)v;
            p = end; matched++;
            break; }
        case 'x': {
            while (isspace((unsigned char)*p)) p++;
            char *end;
            unsigned long v = strtoul(p, &end, 16);
            if (end == p) goto done;
            *va_arg(ap, unsigned int *) = (unsigned int)v;
            p = end; matched++;
            break; }
        case 's': {
            while (isspace((unsigned char)*p)) p++;
            char *dst = va_arg(ap, char *);
            while (*p && !isspace((unsigned char)*p)) *dst++ = *p++;
            *dst = '\0'; matched++;
            break; }
        case 'c':
            *va_arg(ap, char *) = *p++;
            matched++;
            break;
        default:
            break;
        }
    }
done:
    va_end(ap);
    return matched;
}

int fscanf(FILE *fp, const char *fmt, ...)
{
    /* Read one line and sscanf it — good enough for Phase 7 */
    char line[512];
    if (!fgets(line, sizeof(line), fp)) return EOF;
    va_list ap; va_start(ap, fmt);
    /* Build a temp fmt call — delegate to sscanf via direct format match */
    int r = 0;
    const char *p = line;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { if (*p && *p == *f) p++; continue; }
        f++;
        switch (*f) {
        case 'd': { char *e; *va_arg(ap, int *) = (int)strtol(p, &e, 10); p = e; r++; break; }
        case 's': { while (isspace((unsigned char)*p)) p++;
                    char *dst = va_arg(ap, char *);
                    while (*p && !isspace((unsigned char)*p)) *dst++ = *p++;
                    *dst = '\0'; r++; break; }
        default: break;
        }
    }
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...)
{
    char line[512];
    if (!fgets(line, sizeof(line), stdin)) return EOF;
    va_list ap; va_start(ap, fmt);
    /* Delegate via direct sscanf logic */
    int r = 0;
    const char *p = line;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { if (*p && *p == *f) p++; continue; }
        f++;
        switch (*f) {
        case 'd': { char *e; *va_arg(ap, int *) = (int)strtol(p, &e, 10); p = e; r++; break; }
        case 's': { while (isspace((unsigned char)*p)) p++;
                    char *dst = va_arg(ap, char *);
                    while (*p && !isspace((unsigned char)*p)) *dst++ = *p++;
                    *dst = '\0'; r++; break; }
        default: break;
        }
    }
    va_end(ap);
    return r;
}

/* ── Miscellaneous ───────────────────────────────────────────────────── */

FILE *tmpfile(void) { errno = ENOSYS; return NULL; }

int remove(const char *path)
{
    long r = sys_fs_rm(path);
    if (r < 0) { errno = EIO; return -1; }
    return 0;
}

int rename(const char *oldpath, const char *newpath)
{
    /* AetherOS VFS doesn't support rename yet */
    (void)oldpath; (void)newpath;
    errno = ENOSYS; return -1;
}
