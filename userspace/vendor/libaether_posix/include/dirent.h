#ifndef _POSIX_DIRENT_H
#define _POSIX_DIRENT_H

#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4
#define DT_LNK     10

struct dirent {
    ino_t  d_ino;
    off_t  d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char   d_name[256];
};

typedef struct _posix_dir DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *dp);
void           rewinddir(DIR *dp);
int            closedir(DIR *dp);

#endif /* _POSIX_DIRENT_H */
