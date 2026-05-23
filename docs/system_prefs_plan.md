# AetherOS — System Preferences Application
## Development Plan

**Created:** 2026-05-20  
**Status:** SP1–SP6 Complete — SP7 (Integration Testing) pending  
**Branch:** wmanager (continuing from WM8)

---

## Overview

A macOS-style "System Preferences" application (`/sys_prefs`) that manages all
system-wide settings from one place. Settings are persisted to the FAT32 disk
at `/config/` with code fallbacks when files are absent.

**Initial panes:**
- **Display** — resolution presets (applies on reboot)
- **Network** — DHCP vs. static IP/mask/gateway/DNS (static applies immediately)
- **Users** — account management with admin and user roles

**App icon:** `icon_hardware.bmp` (already in `assets/icons/`)  
**Dock slot:** added between Files and the browser.

---

## Architecture Diagram

```
┌────────────────────────────────────────────────────────────────┐
│  userspace/apps/sys_prefs/main.c                               │
│  ┌──────────┐  ┌───────────────────────────────────────────┐   │
│  │ Sidebar  │  │ Content Pane (switches based on selection) │   │
│  │ Display  │  │  Display: resolution listview + Apply btn  │   │
│  │ Network  │  │  Network: DHCP/Static + IP fields          │   │
│  │ Users    │  │  Users:   user list + add/delete/password  │   │
│  └──────────┘  └───────────────────────────────────────────┘   │
└─────────────────────────────┬──────────────────────────────────┘
                              │ syscalls
         ┌────────────────────┼────────────────────┐
         ▼                    ▼                    ▼
   SYS_DISPLAY_*        SYS_NET_CONF_*       SYS_USER_*
   (930-931)            (932-933)            (940-946)
         │                    │                    │
         ▼                    ▼                    ▼
  kernel/core/         kernel/core/         kernel/core/
  (ramfb.c +           (net_init +          users.c
   syscall.c)           syscall.c)
         │                    │                    │
         └────────────────────┴────────────────────┘
                              │
                    /config/ on FAT32
                    ├── display.conf
                    ├── network.conf
                    └── users.conf
```

---

## Syscall Allocation

| Number | Name                  | Args                                 | Return          |
|--------|-----------------------|--------------------------------------|-----------------|
| 930    | SYS_DISPLAY_GET_RES   | —                                    | `(w<<32)\|h`   |
| 931    | SYS_DISPLAY_SET_RES   | `w, h`                               | 0 / -1          |
| 932    | SYS_NET_CONF_GET      | `net_conf_t *out`                    | 0 / -1          |
| 933    | SYS_NET_CONF_SET      | `const net_conf_t *cfg`              | 0 / -1          |
| 940    | SYS_USER_LIST         | `user_info_t *arr, u32 max`          | count / -1      |
| 941    | SYS_USER_CREATE       | `const char *name, const char *pw, u8 role` | uid / -1 |
| 942    | SYS_USER_DELETE       | `u32 uid`                            | 0 / -1          |
| 943    | SYS_USER_SET_PW       | `u32 uid, const char *old, const char *new` | 0 / -1 |
| 944    | SYS_USER_GET_CUR      | `user_info_t *out`                   | uid / -1        |
| 945    | SYS_USER_SET_ROLE     | `u32 uid, u8 role`                   | 0 / -1          |
| 946    | SYS_USER_LOGIN        | `const char *name, const char *pw`   | 0 / -1          |

---

## Config File Formats

### `/config/display.conf`
```
# AetherOS display configuration
width=1280
height=720
```

### `/config/network.conf`
```
# AetherOS network configuration
# mode: dhcp | static
mode=dhcp
ip=0.0.0.0
mask=255.255.255.0
gateway=0.0.0.0
dns=8.8.8.8
```

### `/config/users.conf`
```
# AetherOS user accounts
# format: name:pw_hash:role  (role: 0=user 1=admin)
admin:5381:1
```
Password hash uses djb2 — hash of empty string is 5381 (the seed value).

---

## Phase SP1 — Config Infrastructure
**Status:** ✅ Complete

### SP1.1 — Kernel config reader/writer
**File:** `kernel/core/config.c` (new)  
**Header:** `kernel/include/aether/config.h` (new)

```c
// Read the value for `key` from a key=value config file on FAT32.
// Returns 0 on success, -1 if file not found or key not found.
int kconfig_read(const char *path, const char *key, char *out, u32 out_len);

// Write or update `key=value` in a key=value config file on FAT32.
// Creates the file if it doesn't exist.
// Returns 0 on success, -1 on error.
int kconfig_write(const char *path, const char *key, const char *value);
```

Implementation notes:
- `kconfig_read`: use `fat32_open` → read line-by-line → match `key=` prefix → copy value
- `kconfig_write`: read whole file into a 2KB static buffer → replace/append key → `fat32_create` + `fat32_write`
- Lines starting with `#` are comments; skip them
- Max key length: 32 chars; max value length: 128 chars; max file size: 2048 bytes

### SP1.2 — FAT32 disk setup
**File:** `scripts/make_disk.sh` (modified)

Add:
```bash
mmd -i "${DISK}" ::config
# Write default display.conf
printf "width=1280\nheight=720\n" | mcopy -i "${DISK}" - ::config/display.conf
# Write default network.conf
printf "mode=dhcp\nip=0.0.0.0\nmask=255.255.255.0\ngateway=0.0.0.0\ndns=8.8.8.8\n" | mcopy -i "${DISK}" - ::config/network.conf
# users.conf is NOT pre-created; kernel creates default admin on first boot
```

### SP1.3 — Userspace config library
**File:** `userspace/lib/include/config.h` (new)  
**File:** `userspace/lib/config.c` (new)

```c
// Read key from /config/<file>.conf using VFS syscalls.
// Returns 0 on success, -1 if not found.
int cfg_read(const char *path, const char *key, char *out, int out_len);

// Write key=value to /config/<file>.conf.
// Returns 0 on success, -1 on error.
int cfg_write(const char *path, const char *key, const char *value);
```

Implementation: uses `sys_fs_open` / `sys_fs_read` / `sys_fs_create` / `sys_fs_write`.

---

## Phase SP2 — Display Configuration
**Status:** ✅ Complete  
**Depends on:** SP1

### SP2.1 — Dynamic ramfb resolution at boot

**File:** `kernel/drivers/video/ramfb.c` (modified)  
**File:** `kernel/core/main.c` (modified)

Changes:
- Change `RAMFB_WIDTH/RAMFB_HEIGHT` from `#define` to mutable globals:
  ```c
  static u32 g_ramfb_w = 1280;
  static u32 g_ramfb_h = 720;
  static u16 g_ramfb_selector = 0;  // saved fw_cfg selector for reconfigure
  ```
- Save `selector` from `fwcfg_find_file` to a static global in ramfb.c.
- Add `ramfb_reconfigure(u32 w, u32 h)`:
  - Allocate new physical pages for `w * h * 4` bytes
  - Write new `ramfb_cfg` to fw_cfg via `fwcfg_write_file(g_ramfb_selector, ...)`
  - Update `fb_base, fb_width, fb_height, fb_stride` globals
  - Clear new buffer to background color
- In `kernel_main.c`: after `fat32_mount()` + `vfs_init()` (before `process_spawn`):
  ```c
  // Apply saved display config before spawning init
  char wbuf[8], hbuf[8];
  if (kconfig_read("/config/display.conf", "width", wbuf, 8) == 0 &&
      kconfig_read("/config/display.conf", "height", hbuf, 8) == 0) {
      u32 cw = (u32)katoi(wbuf);
      u32 ch = (u32)katoi(hbuf);
      if (cw != fb_width || ch != fb_height)
          ramfb_reconfigure(cw, ch);
  }
  ```
- Add `katoi()` (kernel atoi) to `kernel/core/printk.c` or a new `kernel/core/kutil.c`.

**Supported resolutions:** 640×480, 800×600, 1024×768, 1280×720, 1920×1080.  
**Note:** QEMU ramfb device supports re-configuration via fw_cfg writes at any point. The old FB memory becomes orphaned (acceptable in a hobby OS; we do not have a pmm_free yet).

### SP2.2 — Display syscalls

**File:** `kernel/include/aether/syscall.h` (modified)  
**File:** `kernel/core/syscall.c` (modified)  
**File:** `userspace/lib/include/sys.h` (modified)

```c
#define SYS_DISPLAY_GET_RES  930  /* () → (fb_width << 32) | fb_height */
#define SYS_DISPLAY_SET_RES  931  /* (w, h) → 0 or -1; writes /config/display.conf */
```

`SYS_DISPLAY_SET_RES` validates the resolution against the allowed list, writes to
`/config/display.conf`, and returns 0. The caller is responsible for showing the
"Restart to apply" message. It does NOT call `ramfb_reconfigure()` at runtime
(would invalidate all running compositor buffers).

Userspace wrappers in `sys.h`:
```c
static inline long sys_display_get_res(u32 *w, u32 *h);
static inline long sys_display_set_res(u32 w, u32 h);
```

---

## Phase SP3 — Network Configuration
**Status:** ✅ Complete  
**Depends on:** SP1

### SP3.1 — Kernel net config support

**File:** `kernel/include/aether/net.h` (modified)  
**File:** `kernel/core/net_conf.c` (new, or added to net subsystem)

Add `net_conf_t`:
```c
typedef struct {
    u8  mode;      /* 0 = DHCP, 1 = static */
    u32 ip;        /* host byte order */
    u32 mask;
    u32 gateway;
    u32 dns;
    u8  mac[6];    /* read-only; set by driver */
    u8  ready;     /* 1 if network is up */
} net_conf_t;
```

In `net_init()` (called from `kernel_main.c` at step 10, AFTER fat32_mount —
**note: currently net_init is called BEFORE fat32_mount; swap the order**):

```
Current order:  net_init → fat32_mount → vfs_init
New order:      fat32_mount → vfs_init → net_init
```

This allows `net_init()` to read `/config/network.conf` before deciding DHCP vs. static.

```c
void net_init(void) {
    virtio_net_init();  // sets g_our_mac
    
    char mode[8];
    if (kconfig_read("/config/network.conf", "mode", mode, 8) == 0
        && kstr_eq(mode, "static")) {
        // Read static config
        kconfig_read("/config/network.conf", "ip",      ...);
        kconfig_read("/config/network.conf", "mask",    ...);
        kconfig_read("/config/network.conf", "gateway", ...);
        kconfig_read("/config/network.conf", "dns",     ...);
        // Parse IPs and set globals
        g_our_ip     = net_ip_parse(ip_str);
        g_gateway_ip = net_ip_parse(gw_str);
        g_subnet_mask= net_ip_parse(mask_str);
        g_dns_ip     = net_ip_parse(dns_str);
        g_net_ready  = 1;
    } else {
        dhcp_init();  // DHCP fallback
    }
}
```

### SP3.2 — Network config syscalls

**File:** `kernel/include/aether/syscall.h` (modified)  
**File:** `kernel/core/syscall.c` (modified)  
**File:** `userspace/lib/include/sys.h` (modified)

```c
#define SYS_NET_CONF_GET  932  /* (net_conf_t *out) → 0 or -1 */
#define SYS_NET_CONF_SET  933  /* (const net_conf_t *cfg) → 0 or -1 */
```

`SYS_NET_CONF_SET` behavior:
- **Static mode**: immediately updates `g_our_ip`, `g_gateway_ip`, `g_subnet_mask`, `g_dns_ip`,
  sets `g_net_ready=1`, writes to `/config/network.conf` → live effect.
- **DHCP mode**: writes to `/config/network.conf` → shows "Restart to apply".

`SYS_NET_CONF_GET`: fills `net_conf_t` from current globals + `g_our_mac` + `g_net_ready`.

---

## Phase SP4 — User Management Kernel
**Status:** ✅ Complete  
**Depends on:** SP1

### SP4.1 — User subsystem

**File:** `kernel/core/users.c` (new)  
**Header:** `kernel/include/aether/users.h` (new)

```c
#define AETHER_MAX_USERS   16
#define AETHER_ROLE_USER    0
#define AETHER_ROLE_ADMIN   1

typedef struct {
    char name[32];
    u32  pw_hash;   /* djb2 hash of password */
    u8   role;      /* AETHER_ROLE_USER / AETHER_ROLE_ADMIN */
    u8   active;    /* 1 = valid slot */
} user_t;

/* Exposed to syscall.c only */
extern user_t g_users[AETHER_MAX_USERS];
extern u32    g_user_count;
extern int    g_current_uid;   /* index into g_users; -1 = not logged in */

void users_init(void);         /* called from kernel_main after vfs_init */
u32  users_djb2(const char *s);
int  users_save(void);         /* write g_users to /config/users.conf */
```

**Password hashing** — djb2:
```c
u32 users_djb2(const char *s) {
    u32 h = 5381;
    while (*s) h = ((h << 5) + h) + (u8)*s++;
    return h;
}
```

**users_init():**
- Try `kconfig_read("/config/users.conf", ...)` with a line-by-line parser
- Config format: one user per line `name:hash:role`
- Fallback if missing: create `g_users[0] = { "admin", 5381, ADMIN, 1 }` and `g_current_uid = 0`
- After loading, if no admin user exists, inject the default admin

**users_save():**
- Build a string of `name:hash:role\n` lines
- `fat32_create("/config/users.conf")` then `fat32_write(...)`

### SP4.2 — User syscalls

**File:** `kernel/core/syscall.c` + `kernel/include/aether/syscall.h`

Userspace-visible struct (in `sys.h`):
```c
typedef struct {
    unsigned int uid;
    char         name[32];
    unsigned char role;    /* 0=user, 1=admin */
} user_info_t;
```

Syscall handlers:
- `SYS_USER_LIST (940)`: copy `g_user_count` `user_info_t` structs to user buffer; returns count
- `SYS_USER_CREATE (941)`: caller must be admin (`g_current_uid` role check); validate unique name; add slot; `users_save()`
- `SYS_USER_DELETE (942)`: caller must be admin; cannot delete self; mark `active=0`; `users_save()`
- `SYS_USER_SET_PW (943)`: verify `old_pw` hash matches; set new hash; `users_save()`; admin can skip old_pw for other users
- `SYS_USER_GET_CUR (944)`: return `g_current_uid`; fill `user_info_t` for current user
- `SYS_USER_SET_ROLE (945)`: caller must be admin; cannot demote self if only admin; `users_save()`
- `SYS_USER_LOGIN (946)`: find user by name; compare djb2(pw) to stored hash; set `g_current_uid`; return 0 or -1

### SP4.3 — Kernel main integration

**File:** `kernel/core/main.c`

Add `users_init()` call after `vfs_init()`:
```c
users_init();
boot_prof_stamp("users");
```

---

## Phase SP5 — System Preferences App
**Status:** ✅ Complete  
**Depends on:** SP1, SP2, SP3, SP4

### SP5.1 — App skeleton, window, sidebar

**File:** `userspace/apps/sys_prefs/main.c` (new)

Window layout (700 × 500):
```
┌──────────────────────────────────────────────────────────┐
│  ░ System Preferences                               × □ ─ │ ← title bar (28px)
├──────────┬───────────────────────────────────────────────┤
│ [Monitor]│  Display                                      │
│ Display  │                                               │
│          │  Current: 1280 × 720                         │
│ [Globe]  │                                               │
│ Network  │  ┌──────────────────────────────────────┐    │
│          │  │ ● 640 × 480                          │    │
│ [Person] │  │   800 × 600                          │    │
│ Users    │  │ ● 1024 × 768                         │    │
│          │  │ ● 1280 × 720  (recommended)          │    │
│          │  │   1920 × 1080                        │    │
│          │  └──────────────────────────────────────┘    │
│          │                                               │
│          │  [  Apply  ]  Changes take effect on restart  │
└──────────┴───────────────────────────────────────────────┘
```

Sidebar is drawn as a custom panel (not libwidget), with:
- Icon (procedural 32×32 glyph) + label
- Highlight on selected pane

Content area uses libwidget for interactive elements.

### SP5.2 — Display pane

Widgets:
- `WIDGET_LABEL` showing "Current resolution: W × H"
- `WIDGET_LISTVIEW` with 5 resolution preset entries
- `WIDGET_BUTTON` "Apply" → calls `sys_display_set_res(w, h)` → shows a small status label

Resolution presets:
```c
static const struct { u32 w, h; const char *label; } k_res[] = {
    { 640,  480, "640 × 480" },
    { 800,  600, "800 × 600" },
    { 1024, 768, "1024 × 768" },
    { 1280, 720, "1280 × 720  (default)" },
    { 1920, 1080, "1920 × 1080" },
};
```

### SP5.3 — Network pane

Widgets:
- Two `WIDGET_CHECKBOX` acting as radio buttons: "DHCP" / "Manual"
- `WIDGET_LABEL` + `WIDGET_TEXTINPUT` for IP, Subnet, Gateway, DNS (4 rows)
- TextInputs disabled (grayed) when DHCP is selected
- `WIDGET_BUTTON` "Apply" → calls `sys_net_conf_set()`
- `WIDGET_LABEL` status line: "Network: connected · 192.168.1.x"

On load: call `sys_net_conf_get()` to populate fields.  
When DHCP→Manual: show current IP in the fields as a starting point.  
When Manual→DHCP: warn "DHCP will apply after restart."

### SP5.4 — Users pane

Widgets:
- `WIDGET_LISTVIEW` listing all users (name + role badge)
- `WIDGET_BUTTON` "Add User" (admin only; shows an inline form below list)
- `WIDGET_BUTTON` "Delete" (admin only; disabled when current user selected)
- `WIDGET_BUTTON` "Change Password" (any user for self; admin for others)

Inline "Add User" form (slides in below list):
- `WIDGET_TEXTINPUT` for username
- `WIDGET_TEXTINPUT` for password (masked with `*`)
- `WIDGET_CHECKBOX` "Admin role"
- `WIDGET_BUTTON` "Create" / "Cancel"

Role enforcement: Non-admin users see the pane but all mutation buttons are grayed.

---

## Phase SP6 — Dock and Build System Integration
**Status:** ✅ Complete  
**Depends on:** SP5

### SP6.1 — Dock entry

**File:** `userspace/apps/dock/main.c` (modified)

Add to `g_dock[]`:
```c
{ "/sys_prefs", "icon_hardware", 0 },
```

Add procedural fallback `draw_icon_hardware()` (gear shape using circles + arc segments).

### SP6.2 — App manifest

**File:** `scripts/make_disk.sh` (modified)

```bash
printf "name=System Preferences\nexec=/sys_prefs\nicon=icon_hardware\n" \
    | mcopy -i "${DISK}" - ::apps/sys_prefs.app
```

### SP6.3 — CMake build target

**File:** `userspace/CMakeLists.txt` (modified)

```cmake
add_executable(user_sys_prefs apps/sys_prefs/main.c userspace/lib/config.c)
target_include_directories(user_sys_prefs PRIVATE lib/include)
target_compile_options(user_sys_prefs PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_sys_prefs PRIVATE libwidget libaether)
target_link_options(user_sys_prefs PRIVATE ${USER_LINK_FLAGS})
set_target_properties(user_sys_prefs PROPERTIES OUTPUT_NAME "sys_prefs" SUFFIX "")
```

Add `user_sys_prefs` to the initrd target dependencies and `INITRD_FILES` list.

---

## Phase SP7 — Integration Testing
**Status:** ☐ Not started — next step  
**Depends on:** SP6

### SP7.1 — Test checklist

**Display:**
- [ ] Open System Preferences → Display pane shows current 1280×720
- [ ] Select 800×600 → Apply → status shows "Changes take effect after restart"
- [ ] Reboot → kernel applies 800×600 from `/config/display.conf`
- [ ] Delete `/config/display.conf` → reboot → falls back to 1280×720

**Network:**
- [ ] Network pane shows current DHCP-assigned IP
- [ ] Switch to Manual → enter static 10.0.2.15/24 gw 10.0.2.2 dns 8.8.8.8 → Apply
- [ ] Verify ping works after static config applied
- [ ] Switch back to DHCP → message "Restart to apply" shown
- [ ] Reboot → DHCP re-runs successfully

**Users:**
- [ ] Pane shows default "admin" user with Admin badge
- [ ] Add user "celso" with role User → appears in list
- [ ] Change password for "celso" → verify new password accepted
- [ ] Try to delete "admin" as admin → prevented (last admin check)
- [ ] `/config/users.conf` on disk has both users after app closed
- [ ] Delete `/config/users.conf` → reboot → default admin re-created

---

## Implementation Order and Dependencies

```
SP1.1 (kconfig kernel)
SP1.2 (make_disk /config)    ← can be done in parallel with SP1.1
SP1.3 (userspace cfg lib)    ← needs SP1.1 concept, not the code

SP2.1 (dynamic ramfb)        ← needs SP1.1
SP2.2 (display syscalls)     ← needs SP2.1

SP3.1 (net conf kernel)      ← needs SP1.1; also reorders kernel boot sequence
SP3.2 (net conf syscalls)    ← needs SP3.1

SP4.1 (users kernel)         ← needs SP1.1
SP4.2 (user syscalls)        ← needs SP4.1
SP4.3 (kernel main)          ← needs SP4.1

SP5.1 (app skeleton)         ← needs SP2.2, SP3.2, SP4.2 (syscalls in sys.h)
SP5.2 (display pane)         ← needs SP5.1, SP2.2
SP5.3 (network pane)         ← needs SP5.1, SP3.2
SP5.4 (users pane)           ← needs SP5.1, SP4.2

SP6.1 (dock entry)           ← needs SP5.1 (app must exist)
SP6.2 (manifest)             ← needs SP5.1
SP6.3 (CMake)                ← needs SP5.1

SP7.1 (testing)              ← needs all above
```

---

## Key Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| ramfb re-config fails at runtime | Test fw_cfg write after initial boot in QEMU; old FB memory is just wasted |
| Reordering fat32_mount before net_init breaks boot | Run both paths in QEMU first; boot_prof timestamps will show any regression |
| djb2 hash collision on passwords | Acceptable for hobby OS; note in code |
| VFS write fails if FAT32 not mounted | All kconfig_write calls check return value and klog a warning |
| User struct changes break initrd apps | `user_info_t` is a new struct; existing apps don't use it |
| Dock array too small | Check `DOCK_MAX` in dock/main.c and increase if needed |

---

## Files to Create (New)

```
kernel/core/config.c
kernel/core/users.c
kernel/include/aether/config.h
kernel/include/aether/users.h
userspace/apps/sys_prefs/main.c
userspace/lib/config.c
userspace/lib/include/config.h
```

## Files to Modify (Existing)

```
kernel/core/main.c           — boot order change; call users_init, ramfb_reconfigure
kernel/core/syscall.c        — 10 new syscall handlers
kernel/include/aether/syscall.h — syscall number defines
kernel/include/aether/net.h  — net_conf_t struct
kernel/drivers/video/ramfb.c — dynamic resolution + reconfigure fn
userspace/lib/include/sys.h  — syscall wrappers + user_info_t + net_conf_t
userspace/apps/dock/main.c   — add sys_prefs entry
userspace/CMakeLists.txt     — user_sys_prefs target
scripts/make_disk.sh         — /config/ dir + sys_prefs.app manifest
```

---

## Progress Log

| Date       | Phase    | Notes |
|------------|----------|-------|
| 2026-05-20 | Plan     | Initial plan created |
| 2026-05-20 | SP1      | `kconfig_read/write`, `katoi/kitoa` in `kernel/core/config.c`; userspace `lib/config.c`; `make_disk.sh` updated with `/config/` dir and defaults |
| 2026-05-20 | SP2      | `ramfb_reconfigure()` added (saves fw_cfg selector as static); `kernel/core/main.c` reads `/config/display.conf` at boot before process_spawn; `SYS_DISPLAY_GET_RES/SET_RES` (930-931) in syscall.c; `sys.h` wrappers |
| 2026-05-20 | SP3      | `net_conf_t` added to `net.h`; `net_init()` reads `/config/network.conf`; boot sequence reordered: fat32_mount → vfs_init → net_init; `SYS_NET_CONF_GET/SET` (932-933) |
| 2026-05-20 | SP4      | `kernel/core/users.c` + `users.h`: `users_init()` (djb2 hash, fallback default admin), `users_save()`; 7 user syscalls (940-946); `kernel/core/main.c` calls `users_init()` after `vfs_init()` |
| 2026-05-20 | SP5      | `userspace/apps/sys_prefs/main.c`: 720×520 window, sidebar (160px), three panes (Display/Network/Users); pane-switch via `g_pane_switch` outer loop; root panel full-width for sidebar click interception |
| 2026-05-20 | SP6      | Dock: 9th slot `{ "/sys_prefs", "icon_hardware", 0 }` + `draw_icon_hardware()` fallback; `userspace/CMakeLists.txt` target `user_sys_prefs`; `make_disk.sh` adds `sys_prefs.app` manifest |
