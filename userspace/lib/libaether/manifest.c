/*
 * AetherOS — App manifest loader
 * File: userspace/lib/libaether/manifest.c
 */

#include <manifest.h>
#include <string.h>
#include <sys.h>

static void parse_kv(const char *buf, long len, manifest_t *m)
{
    const char *p   = buf;
    const char *end = buf + len;

    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;

        const char *eq = p;
        while (eq < nl && *eq != '=') eq++;
        if (eq >= nl) { p = nl + 1; continue; }

        int klen = (int)(eq - p);
        int vlen = (int)(nl - eq - 1);
        if (vlen < 0) vlen = 0;
        const char *val = eq + 1;

        if (klen == 4 && memcmp(p, "name", 4) == 0) {
            int n = vlen < MANIFEST_NAME_MAX - 1 ? vlen : MANIFEST_NAME_MAX - 1;
            memcpy(m->name, val, (unsigned long)n);
            m->name[n] = '\0';
        } else if (klen == 4 && memcmp(p, "icon", 4) == 0) {
            int n = vlen < MANIFEST_ICON_MAX - 1 ? vlen : MANIFEST_ICON_MAX - 1;
            memcpy(m->icon, val, (unsigned long)n);
            m->icon[n] = '\0';
        } else if (klen == 4 && memcmp(p, "exec", 4) == 0) {
            int n = vlen < MANIFEST_EXEC_MAX - 1 ? vlen : MANIFEST_EXEC_MAX - 1;
            memcpy(m->exec, val, (unsigned long)n);
            m->exec[n] = '\0';
        } else if (klen == 11 && memcmp(p, "description", 11) == 0) {
            int n = vlen < MANIFEST_DESC_MAX - 1 ? vlen : MANIFEST_DESC_MAX - 1;
            memcpy(m->desc, val, (unsigned long)n);
            m->desc[n] = '\0';
        }

        p = nl + 1;
    }
}

int manifest_load(const char *path, manifest_t *m)
{
    memset(m, 0, sizeof(*m));

    long vfd = sys_fs_open(path);
    if (vfd < 0) return -1;

    char buf[256];
    long n = sys_fs_read(vfd, buf, (long)sizeof(buf) - 1);
    sys_fs_close(vfd);

    if (n <= 0) return -1;
    buf[n] = '\0';

    parse_kv(buf, n, m);
    return (m->exec[0] != '\0') ? 0 : -1;
}

void manifest_scan_dir(manifest_cb_t cb, void *user_data)
{
    char dirbuf[2048];
    long n = sys_fs_readdir("/apps", dirbuf, (long)sizeof(dirbuf) - 1);
    if (n <= 0) return;
    dirbuf[n] = '\0';

    const char *p   = dirbuf;
    const char *end = p + n;

    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;

        if (*p != '[') {
            const char *sp = p;
            while (sp < nl && *sp != ' ') sp++;
            int flen = (int)(sp - p);

            if (flen > 4 && flen < 48 &&
                memcmp(sp - 4, ".app", 4) == 0) {
                char path[64];
                memcpy(path, "/apps/", 6);
                memcpy(path + 6, p, (unsigned long)flen);
                path[6 + flen] = '\0';

                manifest_t m;
                if (manifest_load(path, &m) == 0)
                    cb(&m, user_data);
            }
        }

        p = nl + 1;
    }
}
