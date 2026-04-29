#ifndef AETHER_MANIFEST_H
#define AETHER_MANIFEST_H

#define MANIFEST_NAME_MAX  32
#define MANIFEST_ICON_MAX  24
#define MANIFEST_EXEC_MAX  48
#define MANIFEST_DESC_MAX  64

typedef struct {
    char name[MANIFEST_NAME_MAX];
    char icon[MANIFEST_ICON_MAX];
    char exec[MANIFEST_EXEC_MAX];
    char desc[MANIFEST_DESC_MAX];
} manifest_t;

int manifest_load(const char *path, manifest_t *m);

typedef void (*manifest_cb_t)(const manifest_t *m, void *user_data);
void manifest_scan_dir(manifest_cb_t cb, void *user_data);

#endif /* AETHER_MANIFEST_H */
