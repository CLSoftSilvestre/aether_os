#ifndef _POSIX_STDIO_H
#define _POSIX_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF     (-1)
#define BUFSIZ  4096
#define FOPEN_MAX 32

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _IOFBF 0  /* fully buffered */
#define _IOLBF 1  /* line buffered */
#define _IONBF 2  /* unbuffered */

/* Opaque FILE type — defined in stdio_posix.c */
typedef struct _posix_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Open / close */
FILE  *fopen(const char *path, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *fp);
int    fclose(FILE *fp);
int    fflush(FILE *fp);
void   setbuf(FILE *fp, char *buf);
int    setvbuf(FILE *fp, char *buf, int mode, size_t size);

/* Read / write */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int    fgetc(FILE *fp);
int    fputc(int c, FILE *fp);
char  *fgets(char *s, int n, FILE *fp);
int    fputs(const char *s, FILE *fp);
int    getchar(void);
int    putchar(int c);
int    puts(const char *s);
int    ungetc(int c, FILE *fp);

/* Positioning */
long   ftell(FILE *fp);
int    fseek(FILE *fp, long offset, int whence);
void   rewind(FILE *fp);
int    fgetpos(FILE *fp, long *pos);
int    fsetpos(FILE *fp, const long *pos);

/* Status */
int    feof(FILE *fp);
int    ferror(FILE *fp);
void   clearerr(FILE *fp);

/* Formatted output */
int    printf(const char *fmt, ...);
int    fprintf(FILE *fp, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *fp, const char *fmt, va_list ap);
int    vsprintf(char *buf, const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* Formatted input */
int    scanf(const char *fmt, ...);
int    fscanf(FILE *fp, const char *fmt, ...);
int    sscanf(const char *buf, const char *fmt, ...);

/* Error reporting */
void   perror(const char *s);

/* Temporary files */
FILE  *tmpfile(void);

/* File removal */
int    remove(const char *path);
int    rename(const char *oldpath, const char *newpath);

/* Convenience macros */
#define getc(fp)    fgetc(fp)
#define putc(c, fp) fputc(c, fp)

#endif /* _POSIX_STDIO_H */
