#ifndef _POSIX_SYS_STAT_H
#define _POSIX_SYS_STAT_H

#include <sys/types.h>
#include <time.h>

/* File type bits */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IFCHR  0020000
#define S_IFBLK  0060000

/* Permission bits */
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRGRP  040
#define S_IWGRP  020
#define S_IXGRP  010
#define S_IROTH  04
#define S_IWOTH  02
#define S_IXOTH  01
#define S_IRWXU  (S_IRUSR|S_IWUSR|S_IXUSR)

/* File type test macros */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

struct stat {
    dev_t    st_dev;
    ino_t    st_ino;
    mode_t   st_mode;
    nlink_t  st_nlink;
    uid_t    st_uid;
    gid_t    st_gid;
    dev_t    st_rdev;
    off_t    st_size;
    long     st_blksize;
    long     st_blocks;
    time_t   st_atime;
    time_t   st_mtime;
    time_t   st_ctime;
};

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
int umask(mode_t mask);

#endif /* _POSIX_SYS_STAT_H */
