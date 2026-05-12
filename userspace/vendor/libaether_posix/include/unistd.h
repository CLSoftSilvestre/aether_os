#ifndef _POSIX_UNISTD_H
#define _POSIX_UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* File I/O */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
int     isatty(int fd);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);

/* Process */
pid_t   getpid(void);
pid_t   getppid(void);

/* Sleep */
unsigned int sleep(unsigned int seconds);
int          usleep(unsigned int usec);

/* Positional I/O (AetherOS: ENOSYS — VFS seek added in Phase 7.6) */
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);

/* Working directory */
char  *getcwd(char *buf, size_t size);

/* File access */
int    access(const char *path, int mode);
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

/* POSIX version constants (minimal) */
#define _POSIX_VERSION 200809L

#endif /* _POSIX_UNISTD_H */
