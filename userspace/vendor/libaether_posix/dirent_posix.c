/*
 * libaether_posix/dirent_posix.c — directory enumeration via AetherOS VFS
 *
 * sys_fs_readdir() returns a flat newline-separated list of entry names.
 * We parse that buffer to provide POSIX opendir/readdir/closedir.
 *
 * Limitations:
 *   - d_ino and d_off are always 0 (VFS doesn't expose inode numbers)
 *   - d_type is DT_REG for everything (no directory flag from VFS readdir)
 *   - Maximum of 16 concurrent open DIRs
 *   - Directory listing is loaded entirely into memory on opendir()
 */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys.h>

#define DIR_BUF_SZ 4096
#define MAX_DIRS    16

struct _posix_dir {
    char  buf[DIR_BUF_SZ];  /* raw readdir output */
    char *ptr;               /* current parse position */
    struct dirent ent;
    int   in_use;
};

static struct _posix_dir _dir_pool[MAX_DIRS];

DIR *opendir(const char *path)
{
    struct _posix_dir *dp = NULL;
    for (int i = 0; i < MAX_DIRS; i++) {
        if (!_dir_pool[i].in_use) { dp = &_dir_pool[i]; break; }
    }
    if (!dp) { errno = EMFILE; return NULL; }

    long n = sys_fs_readdir(path, dp->buf, DIR_BUF_SZ - 1);
    if (n < 0) { errno = ENOENT; return NULL; }
    dp->buf[n] = '\0';
    dp->ptr    = dp->buf;
    dp->in_use = 1;
    return (DIR *)dp;
}

struct dirent *readdir(DIR *dirp)
{
    struct _posix_dir *dp = (struct _posix_dir *)dirp;
    if (!dp || !dp->in_use || !*dp->ptr) return NULL;

    /* Find end of line */
    char *eol = dp->ptr;
    while (*eol && *eol != '\n') eol++;

    size_t len = (size_t)(eol - dp->ptr);
    if (len >= sizeof(dp->ent.d_name)) len = sizeof(dp->ent.d_name) - 1;

    memcpy(dp->ent.d_name, dp->ptr, len);
    dp->ent.d_name[len] = '\0';
    dp->ent.d_ino    = 0;
    dp->ent.d_off    = 0;
    dp->ent.d_reclen = (unsigned short)sizeof(struct dirent);
    dp->ent.d_type   = DT_REG;

    dp->ptr = *eol ? eol + 1 : eol;
    return &dp->ent;
}

void rewinddir(DIR *dirp)
{
    struct _posix_dir *dp = (struct _posix_dir *)dirp;
    if (dp && dp->in_use) dp->ptr = dp->buf;
}

int closedir(DIR *dirp)
{
    struct _posix_dir *dp = (struct _posix_dir *)dirp;
    if (!dp || !dp->in_use) { errno = EBADF; return -1; }
    dp->in_use = 0;
    return 0;
}
