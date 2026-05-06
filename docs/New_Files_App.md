
### Milestone 5.6 — Files App: Explorer-style Two-pane File Browser

**Status:** Not started 📋

**Goal:** Reimagine the Files app as AetherOS's primary graphical file browser, inspired by
Windows Explorer and macOS Finder. The app presents a two-pane layout: a collapsible
directory tree on the left showing all mounted drives (FAT32, InitRD, AetherFS), and an icon
grid on the right displaying the contents of the selected path. Folder icons and per-type file
icons make content immediately recognisable. Double-clicking a folder navigates into it;
double-clicking `.txt` opens TextViewer; double-clicking `.as` opens AetherEditor.

**Dependencies:** Phase 5.3 (libwidget), Phase 5.5 (SYS_SPAWN_ARGS, AetherEditor with argv),
Phase 5.2 VFS (readdir over all three mounts). AetherFS (`/afs`) is probed at startup and
silently omitted from the tree if the virtio-blk 1 device is absent.

#### Architecture decision: TreeView in libwidget vs. inline in files app

The left tree pane is implemented as a new `WIDGET_TREEVIEW` type inside `libwidget`.
A tree view is reusable: a future Save-As dialog, a settings navigator, or a project tree in
AetherEditor will all benefit from it. Adding it to libwidget now avoids duplicated code later.

The right icon grid is **not** extracted as a new widget type — its layout and file-domain
semantics (icon dispatch by extension, double-click spawn) are specific enough that a
`WIDGET_PANEL` with a custom draw function is the right level of abstraction for now. A
generic `WIDGET_GRIDVIEW` can be factored out in a later phase if needed.

#### Window layout

```
┌─────────────────────────────────────────────────────────────────────┐
│  ●  Files                                    /docs         [breadcrumb]│
├─────────────────────┬───────────────────────────────────────────────┤
│  ▼ [drive] FAT32(/) │  [folder] apps   [folder] tmp  [folder] scripts │
│    ▶ [folder] docs  │  [txt] readme.txt  [txt] version.txt             │
│    ▶ [folder] apps  │                                                   │
│    ▶ [folder] tmp   │                                                   │
│    ▶ [folder] scripts│                                                  │
│  ▶ [ram] InitRD     │                                                   │
│  ▶ [afs] AetherFS  │                                                   │
├─────────────────────┴───────────────────────────────────────────────┤
│  2 folders · 2 files                                       FAT32 (/) │
└─────────────────────────────────────────────────────────────────────┘
```

Pixel measurements (900 × 600 window, title bar excluded):
- Left tree pane: **220 px** wide; 1px right border in `C_BORDER`
- Right icon pane: **680 px** wide; scrollable vertically
- Bottom status bar: **22 px** tall
- Icon cell (right pane): **80 × 88 px** (48×48 icon centred, 2-line label at 11px below)
- Tree row height: **20 px**; indent per depth level: **14 px**

#### TreeView widget design (libwidget)

The widget uses a **flat visible-row array** rebuilt on every expand/collapse, avoiding
recursive draw complexity. All nodes live in a fixed static pool — no dynamic allocation.

```c
// widget.h additions
#define TVICON_DRIVE_FAT32   0
#define TVICON_DRIVE_INITRD  1
#define TVICON_DRIVE_AFS     2
#define TVICON_FOLDER_CLOSED 3
#define TVICON_FOLDER_OPEN   4
#define TVICON_FILE          5

typedef struct {
    char     label[64];
    uint8_t  icon_type;     // TVICON_* constant
    uint8_t  depth;         // 0 = drive root, 1+ = subdirectory levels
    uint8_t  has_children;  // 1 if this node can be expanded (dirs, drives)
    uint8_t  expanded;
    int      parent_idx;    // index in nodes[], -1 for roots
    void    *userdata;      // points to VFS path string in a static path pool
} tv_node_t;

typedef struct {
    tv_node_t  nodes[128];
    int        node_count;
    int        visible[128];    // indices into nodes[] for currently visible rows
    int        visible_count;
    int        selected;        // index into visible[]
    int        scroll_offset;   // top visible row index
    void     (*on_select)(tv_node_t *node, void *ctx);
    void     (*on_expand)(tv_node_t *node, void *ctx); // app lazily fills children
    void      *cb_ctx;
} treeview_data_t;
```

Rendering per row: `depth × 14px` left indent → expand triangle (▶/▼, 10×10px) if
`has_children` → 14×14px icon → label clipped to pane width. Selected row gets a full-width
`C_ACCENT` highlight at 40% alpha. Rows outside the visible scroll window are skipped.

Event handling:
- `MOUSE_DOWN` → hit-test row → if triangle area clicked: toggle `expanded`, call `on_expand`
  (if newly expanding), call `treeview_rebuild_visible`; else: update `selected`, call `on_select`
- `KEY_DOWN` Up/Down: move selection, scroll if needed; Right: expand selected node;
  Left: collapse selected node or jump to its parent

Lazy loading contract: when `on_expand` fires, the callback must call
`treeview_add_child()` for each sub-entry and nothing else. On re-collapse,
`treeview_remove_children(w, node_idx)` removes those children so memory is reused.

#### Icon grid (right pane) — custom panel draw

The right pane is a `WIDGET_PANEL` with app-registered draw and event callbacks — no new
widget type is added. State lives in the files app's own global structs:

```c
typedef struct {
    char    name[64];
    char    full_path[256];
    uint8_t icon_type;   // FICON_FOLDER, FICON_TXT, FICON_AS, FICON_EXEC, FICON_GENERIC
    uint8_t is_dir;
} file_entry_t;

file_entry_t g_entries[256];
int          g_entry_count;
int          g_grid_scroll_y;   // vertical pixel offset into the grid
int          g_hovered_entry;   // -1 if none
int          g_selected_entry;  // -1 if none
```

Cell layout (80 × 88 px):
- Icon: 48×48px centred horizontally at cell_x + 16, cell_y + 4
- Label line 1: cell_x, cell_y + 56, width 80px, centred, clipped
- Label line 2: cell_x, cell_y + 68, same constraints
- Hover: `C_PANEL_LIGHT` background fill; Selection: `C_ACCENT` at 35% alpha

Double-click uses the same `click_tracker_t` pattern as Phase 5.4 (50-tick window). The
scroll is driven by mouse wheel delta from `sys_mouse_poll()` (1 row = 88px per tick).

#### File and drive icon set

Nine new scalable icon functions added to `userspace/lib/libaether/gfx.c`. All are drawn
entirely from `gfx_fill`/`gfx_rect`/`gfx_char` primitives; an `sz` parameter lets the same
function draw at 14×14 (tree rows) and 48×48 (icon grid):

| Function | Visual description | Used for |
|----------|--------------------|----------|
| `gfx_icon_drive_fat32(x,y,sz)` | Cylinder/HDD silhouette, white label "FAT" | FAT32 root in tree |
| `gfx_icon_drive_initrd(x,y,sz)` | RAM chip outline, label "RAM" | InitRD mount |
| `gfx_icon_drive_afs(x,y,sz)` | Disk outline with "A" glyph in accent colour | AetherFS mount |
| `gfx_icon_folder(x,y,sz)` | Classic two-tone yellow-gold folder tab shape | Closed directory |
| `gfx_icon_folder_open(x,y,sz)` | Folder with front panel shifted up | Expanded dir in tree; active dir in grid |
| `gfx_icon_file_txt(x,y,sz)` | Page outline + three horizontal lines | `.txt` files |
| `gfx_icon_file_as(x,y,sz)` | Page outline + `{}` code symbol in accent | `.as` AetherScript files |
| `gfx_icon_file_exec(x,y,sz)` | Gear/cog silhouette | Executable ELF binaries |
| `gfx_icon_file_generic(x,y,sz)` | Plain page outline, folded top-right corner | Unknown file types |

File type detection in the grid is extension-based: `.txt` → `FICON_TXT`, `.as` → `FICON_AS`,
`.elf` / no extension (executable flag) → `FICON_EXEC`, anything else → `FICON_GENERIC`.

#### VFS mount discovery helper

A lightweight static table lives in a new libaether file so any future app can enumerate mounts:

```c
// userspace/lib/libaether/vfs_mounts.h
typedef struct {
    const char *label;       // "FAT32 (/)"
    const char *path;        // "/"
    uint8_t     icon_type;   // TVICON_DRIVE_* constant
    uint8_t     available;   // set by vfs_probe_mounts()
} mount_info_t;

void vfs_probe_mounts(void);
int  vfs_get_mounts(mount_info_t *out, int max);
```

`vfs_probe_mounts()` tries `sys_fs_readdir(path, buf, sizeof(buf))` on each entry; marks
`available = 1` on success. Run once at files app startup. No new syscall needed.

#### Spawn integration

Files app uses `sys_spawn_args()` from Phase 5.5 to open files in the correct app:

```c
// Double-click .txt
const char *argv_tv[] = { "/bin/textviewer", entry.full_path, NULL };
sys_spawn_args("/bin/textviewer", argv_tv, 2);

// Double-click .as
const char *argv_ed[] = { "/bin/aether_editor", entry.full_path, NULL };
sys_spawn_args("/bin/aether_editor", argv_ed, 2);
```

TextViewer and AetherEditor must be updated to check `argc >= 2` and open `argv[1]` on
startup (see tasks 5.6.6–5.6.7 below).

#### New kernel syscalls (Phase 5.6)

None required. All necessary primitives exist: `SYS_FS_READDIR (803)`, `SYS_SPAWN_ARGS (29)`,
`SYS_WM_REGISTER (12)`, `sys_mouse_poll`, `sys_wm_key_recv`.

#### Tasks

**5.6.1 — TreeView widget**
- [x] **5.6.1** `userspace/lib/include/widget.h`
  - Add `WIDGET_TREEVIEW` to `widget_type_t` enum
  - Add `TVICON_*` constants (6 values: drive_fat32, drive_initrd, drive_afs, folder_closed, folder_open, file)
  - Add `tv_node_t` and `treeview_data_t` structs
  - Declare `treeview_init`, `treeview_add_root`, `treeview_add_child`, `treeview_remove_children`,
    `treeview_set_callbacks`, `treeview_rebuild_visible`
- [x] **5.6.2** `userspace/lib/libwidget/treeview.c`
  - `treeview_init(w, parent, bounds)` — zero-init `treeview_data_t`, register draw/event/tick fns
  - `treeview_add_root(w, label, icon, userdata)` → node index (depth=0, parent=-1)
  - `treeview_add_child(w, parent_idx, label, icon, has_children, userdata)` → node index
  - `treeview_remove_children(w, parent_idx)` — mark child nodes inactive, rebuild visible
  - `treeview_rebuild_visible(w)` — DFS walk of nodes[]; push index to visible[] if
    reachable through a chain of expanded ancestors
  - Draw function: background fill `C_PANEL`; per visible row: compute indent, draw triangle
    (10×10 ▶/▼ or blank), draw icon at 14×14, draw label clipped to right edge; highlight
    selected row with semi-transparent `C_ACCENT` rect; rows outside scroll window skipped
  - Event function: MOUSE_DOWN hit-test → toggle expand or update selection + fire callbacks;
    KEY_DOWN Up/Down/Right/Left navigation; scroll visible rows to keep selection on screen
- [x] **5.6.3** `userspace/lib/libwidget/CMakeLists.txt` — add `treeview.c` to `libwidget` sources

**5.6.2 — File and drive icon set**
- [x] **5.6.4** `userspace/lib/libaether/gfx.c` + `gfx.h` — 9 new icon functions
  - `gfx_icon_drive_fat32(x, y, sz)`, `gfx_icon_drive_initrd(x, y, sz)`, `gfx_icon_drive_afs(x, y, sz)`
  - `gfx_icon_folder(x, y, sz)`, `gfx_icon_folder_open(x, y, sz)`
  - `gfx_icon_file_txt(x, y, sz)`, `gfx_icon_file_as(x, y, sz)`,
    `gfx_icon_file_exec(x, y, sz)`, `gfx_icon_file_generic(x, y, sz)`
  - Each function draws only with `gfx_fill`/`gfx_rect`/`gfx_char`; shapes scale linearly by `sz`
  - Drive icons must be distinguishable at both 14px (tree) and 48px (grid) sizes

**5.6.3 — VFS mount discovery helper**
- [x] **5.6.5** `userspace/lib/libaether/vfs_mounts.c` + `vfs_mounts.h`
  - Static `g_mounts[3]` table: `{"FAT32 (/)", "/", TVICON_DRIVE_FAT32}`,
    `{"InitRD (/initrd)", "/initrd", TVICON_DRIVE_INITRD}`,
    `{"AetherFS (/afs)", "/afs", TVICON_DRIVE_AFS}`
  - `vfs_probe_mounts()` — call `sys_fs_readdir` on each path; set `available` flag
  - `vfs_get_mounts(out, max)` → count of available mounts

**5.6.4 — Files app complete rewrite**
- [x] **5.6.6** `userspace/apps/files/main.c` — full rewrite using libwidget
  - **Setup:** `sys_wm_register(50, 40, 900, 600, "Files")` on startup
  - **Root layout:** root `WIDGET_PANEL` (900×572) containing three regions:
    left panel (0, 0, 220, 548), divider (220, 0, 1, 548), right panel (221, 0, 679, 548), status bar (0, 548, 900, 24)
  - **Left panel:** `WIDGET_TREEVIEW` filling the full left panel
    - `vfs_probe_mounts()` at startup; `treeview_add_root()` per available mount
    - `on_expand` callback: call `sys_fs_readdir(node->userdata, buf, sizeof(buf))`; parse entries;
      call `treeview_add_child()` for each subdirectory (has_children=1) and each file (has_children=0)
    - `on_select` callback: call `files_navigate_to(node->userdata)`
  - **Right panel:** `WIDGET_PANEL` with app-registered draw/event callbacks
    - Custom draw: iterate `g_entries[0..g_entry_count]`; compute cell (col, row) from index and
      pane width; skip cells outside `g_grid_scroll_y` viewport; call appropriate `gfx_icon_*` at 48px;
      draw 2-line clipped label; draw hover/selection backgrounds
    - Custom event: MOUSE_DOWN → hit-test to find entry index → update `g_selected_entry`;
      double-click (50-tick window): folder → `files_navigate_to(entry.full_path)`;
      `.txt` → `sys_spawn_args("/bin/textviewer", ...)`;
      `.as` → `sys_spawn_args("/bin/aether_editor", ...)`
    - Scroll: WEV_MOUSE_WHEEL delta (from mouse poll) → adjust `g_grid_scroll_y` by ±88px, clamp to content
    - KEY_DOWN: arrows navigate `g_selected_entry`; Enter activates; Backspace → parent directory
  - **Breadcrumb / path label:** `WIDGET_LABEL` in title bar strip showing current path, updated after each navigate
  - **Status bar:** `WIDGET_LABEL` showing `"N folders · M files  filesystem_label"`, right-aligned filesystem name
  - **`files_navigate_to(path)`:** clear `g_entries`, call `sys_fs_readdir(path)`, parse entries into
    `g_entries[]`, reset `g_grid_scroll_y = 0`, `g_selected_entry = -1`, update breadcrumb and status labels,
    sync tree selection to active path, `widget_invalidate(root)`

**5.6.5 — TextViewer: open-from-args**
- [x] **5.6.7** `userspace/apps/textviewer/main.c`
  - Check `argc >= 2` on startup; if true, `sys_fs_open(argv[1])` and display file contents immediately
  - If `argc == 1`: show empty state message "Open a file from the Files app"

**5.6.6 — AetherEditor: open-from-args**
- [x] **5.6.8** `userspace/apps/aether_editor/main.c`
  - Check `argc >= 2` on startup; if true, load `argv[1]` into the textarea and set current filename
  - Behaviour is identical to clicking the manual "Open" button and selecting that file

**5.6.7 — Integration test**
- [x] **5.6.9** Boot → double-click Files icon → 900×600 window opens; left tree shows FAT32 (/), InitRD (/initrd); right pane shows FAT32 root icon grid
- [x] **5.6.10** Click `▶` next to `docs` folder in tree → node expands, children appear; click `docs` node → right pane updates to `/docs/` contents; breadcrumb shows `/docs`
- [x] **5.6.11** Double-click a folder in the icon grid → navigates in; tree selection follows; Backspace returns to parent
- [x] **5.6.12** Double-click `readme.txt` in grid → TextViewer opens and displays file contents immediately
- [x] **5.6.13** Double-click `hello.as` in grid → AetherEditor opens with the script pre-loaded in textarea
- [x] **5.6.14** Click InitRD (`/initrd`) in tree → right pane shows initrd files; drive icon distinguishes it from FAT32
- [x] **5.6.15** Status bar shows correct folder/file counts and current filesystem label after each navigation
- [x] **5.6.16** Hover over icon cell → hover highlight; single-click → selection highlight; both clear on navigation

**Phase 5.6 Success Criteria:** Files app launches as a full two-pane explorer; all mounted
drives appear in the tree with distinct drive icons; folders expand/collapse lazily; icon grid
shows per-type icons for folders, text files, scripts, executables, and generic files;
double-clicking a `.txt` opens TextViewer with that file loaded; double-clicking a `.as` opens
AetherEditor with that script loaded; navigation and Backspace work in both panes.

---
