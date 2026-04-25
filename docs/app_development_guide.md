# AetherOS App Developer Guide

## Introduction

AetherOS is a hobby operating system for AArch64 (Raspberry Pi 4/5 and QEMU). Apps run as isolated EL0 user-mode processes, draw directly to a shared framebuffer, and communicate with the kernel exclusively through syscalls. There is no dynamic linking, no windowing protocol, and no libc — each app is a statically linked ELF binary bundled into the initrd.

This guide walks you from zero to a running "Hello World" window app, then builds on that to cover input, drawing, and spawning child processes.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Project Layout](#2-project-layout)
3. [Hello World — Step by Step](#3-hello-world--step-by-step)
   - 3.1 [Create the app directory and source file](#31-create-the-app-directory-and-source-file)
   - 3.2 [Write the app](#32-write-the-app)
   - 3.3 [Register the app in the build system](#33-register-the-app-in-the-build-system)
   - 3.4 [Build and run](#34-build-and-run)
4. [Understanding the App Skeleton](#4-understanding-the-app-skeleton)
   - 4.1 [Startup flow](#41-startup-flow)
   - 4.2 [The main event loop](#42-the-main-event-loop)
   - 4.3 [Exiting cleanly](#43-exiting-cleanly)
5. [Drawing to the Screen](#5-drawing-to-the-screen)
   - 5.1 [The gfx API](#51-the-gfx-api)
   - 5.2 [Color system](#52-color-system)
   - 5.3 [Drawing a standard window chrome](#53-drawing-a-standard-window-chrome)
6. [Handling Input](#6-handling-input)
   - 6.1 [Keyboard events](#61-keyboard-events)
   - 6.2 [Mouse events](#62-mouse-events)
   - 6.3 [Non-blocking polling](#63-non-blocking-polling)
7. [Using libaether (the Standard Library)](#7-using-libaether-the-standard-library)
8. [Syscall Reference](#8-syscall-reference)
9. [Spawning Child Processes](#9-spawning-child-processes)
10. [Complete Example — Counter App](#10-complete-example--counter-app)
11. [Launching Your App from the Desktop](#11-launching-your-app-from-the-desktop)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. Prerequisites

**Toolchain required:**

| Tool | Purpose |
|------|---------|
| `aarch64-elf-gcc` | Cross-compiler (bare-metal AArch64) |
| `aarch64-elf-objcopy` | ELF to binary conversion |
| CMake ≥ 3.20 | Build system |
| Ninja | Build backend |
| QEMU (`qemu-system-aarch64`) | Emulator for testing |

**Install on macOS (Homebrew):**
```bash
brew install aarch64-elf-gcc cmake ninja qemu
```

**Verify the toolchain:**
```bash
aarch64-elf-gcc --version
# aarch64-elf-gcc (GCC) 13.x.x ...
```

---

## 2. Project Layout

```
aether_os/
├── kernel/                     # Kernel sources (do not modify for app dev)
├── userspace/
│   ├── lib/
│   │   ├── include/            # Headers your app includes
│   │   │   ├── gfx.h           # Graphics API
│   │   │   ├── sys.h           # Syscall wrappers
│   │   │   ├── input.h         # Input event types
│   │   │   ├── stdio.h         # printf, snprintf
│   │   │   ├── string.h        # strlen, strcpy, memset
│   │   │   └── stdlib.h        # malloc, free, atoi
│   │   ├── libaether/          # libaether implementation
│   │   ├── crt0.S              # C runtime entry point
│   │   └── user.ld             # Linker script
│   ├── apps/
│   │   ├── init/               # PID 1 — desktop manager
│   │   ├── aether_term/        # Terminal emulator
│   │   ├── statusbar/          # System info sidebar
│   │   ├── files/              # File browser
│   │   ├── textviewer/         # Text pager
│   │   └── YOUR_APP/           # ← you create this
│   └── CMakeLists.txt          # ← you add your app here
├── docs/
│   └── app_development_guide.md
└── CMakeLists.txt
```

**Key headers at a glance:**

| Header | What it gives you |
|--------|------------------|
| `gfx.h` | `gfx_fill`, `gfx_text`, `gfx_char`, `gfx_rect`, color macros |
| `sys.h` | `sys_exit`, `sys_sleep`, `sys_spawn`, `sys_key_read`, etc. |
| `input.h` | `key_event_t`, `mouse_event_t`, keycode constants |
| `stdio.h` | `printf`, `snprintf`, `puts` |
| `string.h` | `strlen`, `strcpy`, `memset`, `strcmp` |
| `stdlib.h` | `malloc`, `free`, `atoi` |

---

## 3. Hello World — Step by Step

### 3.1 Create the app directory and source file

```bash
mkdir -p userspace/apps/hello
touch userspace/apps/hello/main.c
```

### 3.2 Write the app

Open `userspace/apps/hello/main.c` and paste the following:

```c
#include <gfx.h>
#include <sys.h>
#include <input.h>

/* Window geometry */
#define WIN_X    200
#define WIN_Y    150
#define WIN_W    400
#define WIN_H    200
#define TITLE_H  28

int main(void) {
    /* Step 1: initialise the graphics layer */
    gfx_init();

    /* Step 2: claim the framebuffer so the kernel stops writing to it */
    sys_fb_claim();

    /* Step 3: paint the desktop background */
    gfx_fill(0, 0, gfx_width(), gfx_height(), C_DESKTOP);

    /* Step 4: draw a window */

    /* drop shadow */
    gfx_fill(WIN_X + 4, WIN_Y + 4, WIN_W, WIN_H, GFX_RGB(8, 8, 14));

    /* window body */
    gfx_fill(WIN_X, WIN_Y, WIN_W, WIN_H, C_WIN_BG);

    /* title bar */
    gfx_fill(WIN_X, WIN_Y, WIN_W, TITLE_H, C_TITLEBAR);

    /* traffic-light buttons */
    gfx_fill(WIN_X + 10, WIN_Y + 8, 12, 12, C_RED);
    gfx_fill(WIN_X + 26, WIN_Y + 8, 12, 12, C_YELLOW);
    gfx_fill(WIN_X + 42, WIN_Y + 8, 12, 12, C_GREEN);

    /* title text */
    gfx_text_center(WIN_X, WIN_W, WIN_Y + 8, "Hello AetherOS", C_TEXT, C_TITLEBAR);

    /* purple separator under title bar */
    gfx_hline(WIN_X, WIN_Y + TITLE_H, WIN_W, C_ACCENT);

    /* window border */
    gfx_rect(WIN_X, WIN_Y, WIN_W, WIN_H, C_SEP);

    /* Step 5: draw the greeting message */
    gfx_text(WIN_X + 20, WIN_Y + TITLE_H + 30,
             "Hello, World!", C_TEXT, C_WIN_BG);
    gfx_text(WIN_X + 20, WIN_Y + TITLE_H + 50,
             "Press ESC to exit.", C_TEXT_DIM, C_WIN_BG);

    /* Step 6: event loop — wait for ESC */
    for (;;) {
        unsigned long long raw = sys_key_read();
        key_event_t ev = key_event_unpack(raw);

        if (ev.is_press && ev.keycode == KEY_ESC) {
            break;
        }
    }

    sys_exit(0);
    return 0;
}
```

### 3.3 Register the app in the build system

Open `userspace/CMakeLists.txt`. Find the block where the other apps are declared (look for `add_executable(user_aether_term` as a reference) and append:

```cmake
# ── hello ──────────────────────────────────────────────────
add_executable(user_hello
    apps/hello/main.c
    lib/crt0.S
)
target_include_directories(user_hello PRIVATE lib/include)
target_compile_options(user_hello PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_hello PRIVATE libaether)
target_link_options(user_hello PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_hello PROPERTIES
    OUTPUT_NAME "hello"
    SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)
```

Then find the `add_custom_command` that builds `initrd.cpio` and add `user_hello` to its `DEPENDS` list:

```cmake
add_custom_command(
    OUTPUT ${INITRD_CPIO}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/make_initrd.sh ...
    DEPENDS user_init user_aether_term user_statusbar user_files
            user_textviewer user_hello          # ← add this
)
```

### 3.4 Build and run

```bash
cd /path/to/aether_os
mkdir -p build && cd build
cmake -GNinja ..
ninja

# Run in QEMU
qemu-system-aarch64 \
  -M virt -cpu cortex-a53 -m 512M \
  -kernel kernel/aether_kernel.elf \
  -serial stdio \
  -display cocoa            # or -display gtk on Linux
```

You should see the AetherOS desktop. Open a terminal and type:

```
hello
```

Your window appears. Press **ESC** to close it.

---

## 4. Understanding the App Skeleton

### 4.1 Startup flow

When `sys_spawn("/hello")` is called (by init or the terminal), the kernel:

1. Locates the `hello` ELF in the CPIO initrd.
2. Allocates a private page table for the new process.
3. Loads ELF segments into user virtual memory (starting at `0x70000000`).
4. Creates a `task_t` (TCB) and queues it in the scheduler.
5. On the next timer tick the scheduler context-switches into the new task at EL0.

Before `main()` is called, `crt0.S` runs first. It:
- Clears the BSS section.
- Calls `main()`.
- On return, calls `sys_exit(return_value)`.

You never need to touch `crt0.S` — it is linked automatically.

### 4.2 The main event loop

AetherOS uses **cooperative multitasking**. There is no preemption while your app is in user space (the timer interrupt does fire, but it yields to the scheduler only between kernel calls). To keep the system responsive you must yield the CPU regularly.

The idiomatic pattern is a blocking read:

```c
for (;;) {
    unsigned long long raw = sys_key_read();   /* blocks until a key arrives */
    /* process event, redraw if needed */
}
```

`sys_key_read()` suspends your process in the scheduler's BLOCKED state and only returns when a key event is available — no CPU is wasted while waiting.

If your app also needs to update a clock or animation, use `sys_key_poll()` with a sleep:

```c
for (;;) {
    unsigned long long raw = sys_key_poll();
    if (raw != 0) {
        key_event_t ev = key_event_unpack(raw);
        /* handle event */
    }
    /* periodic work here */
    sys_sleep(10);   /* sleep 10 ticks ≈ 100 ms, yields CPU */
}
```

### 4.3 Exiting cleanly

Call `sys_exit(code)` at the end of your app. `crt0.S` does this for you if `main()` returns normally, but it is good practice to call it explicitly. The kernel will:

- Mark the task as ZOMBIE until the parent calls `sys_waitpid`.
- Free the process's page table and physical pages.

---

## 5. Drawing to the Screen

### 5.1 The gfx API

All drawing is done through the graphics layer in `gfx.h`. These are thin wrappers around framebuffer syscalls.

**Initialisation — call once at startup:**
```c
gfx_init();          /* caches screen dimensions from kernel */
sys_fb_claim();      /* disables kernel text console — now you own the FB */
```

**Screen dimensions:**
```c
unsigned w = gfx_width();    /* always 1024 */
unsigned h = gfx_height();   /* always 768  */
```

**Filled rectangles:**
```c
gfx_fill(int x, int y, int w, int h, unsigned color);
```

**Lines:**
```c
gfx_hline(int x, int y, int len, unsigned color);   /* horizontal */
gfx_vline(int x, int y, int len, unsigned color);   /* vertical   */
```

**Rectangle outlines:**
```c
gfx_rect(int x, int y, int w, int h, unsigned color);
```

**Characters and strings** (8×8 pixel font):
```c
gfx_char(int x, int y, char c, unsigned fg, unsigned bg);
gfx_text(int x, int y, const char *s, unsigned fg, unsigned bg);
gfx_text_center(int cx, int cw, int y, const char *s, unsigned fg, unsigned bg);
gfx_printf(int x, int y, unsigned fg, unsigned bg, const char *fmt, ...);
```

**Timing:**
```c
long ticks = gfx_ticks();   /* 100 Hz ticks since boot */
```

### 5.2 Color system

Colors are 24-bit RGB packed into an `unsigned int`. Use the `GFX_RGB` macro or the shared palette constants.

**Constructing a color:**
```c
unsigned red   = GFX_RGB(235, 87,  87);
unsigned white = GFX_RGB(255, 255, 255);
unsigned black = GFX_RGB(0,   0,   0);
```

**Built-in palette** (defined in `gfx.h`):

| Constant | Color | Typical use |
|----------|-------|-------------|
| `C_DESKTOP` | Near-black `#121218` | Desktop background |
| `C_PANEL` | Dark blue-grey `#1a1a28` | Top/bottom bars |
| `C_WIN_BG` | Dark `#141420` | Window content area |
| `C_TITLEBAR` | Dark `#1e1e32` | Window title bar |
| `C_TERM_BG` | Darkest `#0c0c14` | Terminal background |
| `C_ACCENT` | Purple `#7c6af7` | Highlights, separators |
| `C_ACCENT2` | Cyan `#00c8dc` | Secondary highlights |
| `C_TEXT` | Light `#d8d8e8` | Primary text |
| `C_TEXT_DIM` | Grey `#64648c` | Secondary text |
| `C_SEP` | Same as dim | Borders |
| `C_RED` | `#eb5757` | Close button |
| `C_YELLOW` | `#f7c948` | Minimize button |
| `C_GREEN` | `#50c84b` | Maximize button |

Always use palette constants for consistency with the Lumina desktop aesthetic.

### 5.3 Drawing a standard window chrome

Every app is expected to draw its own window. The standard chrome consists of:

1. Drop shadow (offset dark rectangle)
2. Window body
3. Title bar with traffic-light buttons
4. Accent separator line
5. Border outline

The following function implements the standard chrome. Copy it into your app and call it once during initialisation:

```c
#define TITLE_H  28

static void draw_window(int x, int y, int w, int h, const char *title) {
    /* shadow */
    gfx_fill(x + 4, y + 4, w, h, GFX_RGB(8, 8, 14));

    /* body */
    gfx_fill(x, y, w, h, C_WIN_BG);

    /* title bar */
    gfx_fill(x, y, w, TITLE_H, C_TITLEBAR);

    /* traffic lights */
    gfx_fill(x + 10, y + 8, 12, 12, C_RED);
    gfx_fill(x + 26, y + 8, 12, 12, C_YELLOW);
    gfx_fill(x + 42, y + 8, 12, 12, C_GREEN);

    /* title */
    gfx_text_center(x, w, y + 8, title, C_TEXT, C_TITLEBAR);

    /* separator */
    gfx_hline(x, y + TITLE_H, w, C_ACCENT);

    /* border */
    gfx_rect(x, y, w, h, C_SEP);
}
```

The content area for drawing begins at `(x, y + TITLE_H + 1)`.

---

## 6. Handling Input

### 6.1 Keyboard events

**Reading a blocking key event:**
```c
unsigned long long raw = sys_key_read();
key_event_t ev = key_event_unpack(raw);
```

The `key_event_t` struct (defined in `input.h`):
```c
typedef struct {
    unsigned int  keycode;     /* keycode_t enum value  */
    unsigned char modifiers;   /* bitmask, see below    */
    unsigned char is_press;    /* 1 = pressed, 0 = released */
} key_event_t;
```

**Modifier bitmask:**
```c
#define MOD_SHIFT  (1 << 0)
#define MOD_CTRL   (1 << 1)
#define MOD_ALT    (1 << 2)
#define MOD_CAPS   (1 << 3)

/* Example: check Ctrl+C */
if (ev.keycode == KEY_C && (ev.modifiers & MOD_CTRL) && ev.is_press) {
    sys_exit(1);
}
```

**Common key constants** (from `input.h`):

```c
KEY_A … KEY_Z        /* letters        */
KEY_0 … KEY_9        /* digits         */
KEY_ENTER            /* Enter/Return   */
KEY_SPACE            /* Space          */
KEY_BACKSPACE        /* Backspace      */
KEY_ESC              /* Escape         */
KEY_TAB              /* Tab            */
KEY_UP               /* Arrow Up       */
KEY_DOWN             /* Arrow Down     */
KEY_LEFT             /* Arrow Left     */
KEY_RIGHT            /* Arrow Right    */
KEY_F1 … KEY_F12     /* Function keys  */
```

**Converting a keycode to ASCII** (for text input):
```c
char ch = key_to_ascii(ev.keycode, ev.modifiers);
if (ch != 0) {
    /* printable character */
}
```

### 6.2 Mouse events

```c
unsigned long long raw = sys_mouse_read();   /* blocking */
mouse_event_t me = mouse_event_unpack(raw);
```

The `mouse_event_t` struct:
```c
typedef struct {
    unsigned short x;        /* 0–1023  */
    unsigned short y;        /* 0–767   */
    unsigned char  buttons;  /* bitmask */
} mouse_event_t;

#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_MIDDLE (1 << 1)
#define MOUSE_BTN_RIGHT  (1 << 2)
```

**Hit testing example:**
```c
if (me.buttons & MOUSE_BTN_LEFT) {
    if (me.x >= btn_x && me.x < btn_x + btn_w &&
        me.y >= btn_y && me.y < btn_y + btn_h) {
        /* button was clicked */
    }
}
```

### 6.3 Non-blocking polling

Use `sys_key_poll()` / `sys_mouse_poll()` when you need to check for input without blocking:

```c
unsigned long long raw = sys_key_poll();
if (raw != 0) {
    key_event_t ev = key_event_unpack(raw);
    /* handle it */
}
```

Returns `0` immediately if no event is pending. Remember to call `sys_sleep()` in your loop to yield the CPU:

```c
for (;;) {
    unsigned long long kev = sys_key_poll();
    unsigned long long mev = sys_mouse_poll();

    if (kev) { /* ... */ }
    if (mev) { /* ... */ }

    /* update state */
    redraw_if_needed();

    sys_sleep(2);   /* ~20 ms */
}
```

---

## 7. Using libaether (the Standard Library)

libaether is a minimal freestanding C library linked into every app. It lives in `userspace/lib/libaether/` and provides:

**stdio.h**
```c
int printf(const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int puts(const char *s);
```
`printf` outputs to `stdout` (FD 1), which maps to the UART in the current implementation.

**string.h**
```c
size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
void  *memset(void *p, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
```

**stdlib.h**
```c
void *malloc(size_t size);
void  free(void *ptr);
int   atoi(const char *s);
long  strtol(const char *s, char **end, int base);
```

**Limitations:**
- `malloc` uses a simple bump/slab allocator. Avoid frequent small allocations.
- There is no `fopen`/`fclose`. Use `sys_initrd_read` to read files from the initrd.
- No floating-point formatting in `printf` (`%f` is not supported).

---

## 8. Syscall Reference

The syscall wrappers in `sys.h` cover all kernel interfaces apps need.

### Process management

```c
void sys_exit(int code);
long sys_spawn(const char *path);          /* returns child PID or negative error */
int  sys_waitpid(long pid, int *status);   /* blocks until child exits */
long sys_getpid(void);
void sys_sleep(unsigned ticks);            /* 1 tick = 10 ms at 100 Hz */
void sys_yield(void);                      /* yield CPU without sleeping */
```

### Graphics

```c
void sys_fb_claim(void);                   /* take ownership of framebuffer */
```
(All other graphics operations go through `gfx.h` wrappers.)

### Input

```c
unsigned long long sys_key_read(void);     /* blocking key read  */
unsigned long long sys_key_poll(void);     /* non-blocking       */
unsigned long long sys_mouse_read(void);   /* blocking mouse     */
unsigned long long sys_mouse_poll(void);   /* non-blocking       */
```

### File system (initrd)

```c
int  sys_initrd_ls(char *buf, size_t bufsz);                     /* list files      */
long sys_initrd_read(const char *path, void *buf, size_t bufsz); /* read file bytes */
```

### System information

```c
long sys_get_ticks(void);           /* 100 Hz ticks since boot                 */
int  sys_pmm_stats(unsigned *free_pages, unsigned *total_pages); /* memory info */
```

---

## 9. Spawning Child Processes

An app can launch another app using `sys_spawn`:

```c
long child = sys_spawn("/textviewer");
if (child < 0) {
    /* spawn failed */
}

/* Optionally wait for the child to exit */
int status = 0;
sys_waitpid(child, &status);
```

The path is relative to the initrd root (always use a leading `/`).

**Passing arguments:** The current implementation does not support `argv`. If your app needs to receive configuration from its parent, use a pipe (see below) or a naming convention where the parent writes a small config file to a known initrd path before spawning.

**Pipes between processes:**
```c
int fds[2];
sys_pipe(fds);           /* fds[0] = read end, fds[1] = write end */

long child = sys_spawn("/my_helper");

/* parent writes */
sys_write(fds[1], "go\n", 3);

/* child reads from its stdin (FD 0) if you dup2 before spawn */
```

---

## 10. Complete Example — Counter App

This example shows a window with a counter that increments every second, and can be incremented manually by pressing `+` or decremented by pressing `-`. Press `Q` to quit.

```c
/* userspace/apps/counter/main.c */

#include <gfx.h>
#include <sys.h>
#include <input.h>
#include <stdio.h>
#include <string.h>

#define WIN_X    300
#define WIN_Y    200
#define WIN_W    300
#define WIN_H    180
#define TITLE_H  28
#define CONTENT_Y (WIN_Y + TITLE_H + 1)

static int counter = 0;
static long last_tick = 0;

static void draw_window_chrome(void) {
    gfx_fill(WIN_X + 4, WIN_Y + 4, WIN_W, WIN_H, GFX_RGB(8, 8, 14));
    gfx_fill(WIN_X, WIN_Y, WIN_W, WIN_H, C_WIN_BG);
    gfx_fill(WIN_X, WIN_Y, WIN_W, TITLE_H, C_TITLEBAR);
    gfx_fill(WIN_X + 10, WIN_Y + 8, 12, 12, C_RED);
    gfx_fill(WIN_X + 26, WIN_Y + 8, 12, 12, C_YELLOW);
    gfx_fill(WIN_X + 42, WIN_Y + 8, 12, 12, C_GREEN);
    gfx_text_center(WIN_X, WIN_W, WIN_Y + 8, "Counter", C_TEXT, C_TITLEBAR);
    gfx_hline(WIN_X, WIN_Y + TITLE_H, WIN_W, C_ACCENT);
    gfx_rect(WIN_X, WIN_Y, WIN_W, WIN_H, C_SEP);
}

static void redraw_counter(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Count: %d", counter);

    /* clear content area */
    gfx_fill(WIN_X + 1, CONTENT_Y, WIN_W - 2, WIN_H - TITLE_H - 1, C_WIN_BG);

    /* draw counter value */
    gfx_text_center(WIN_X, WIN_W, CONTENT_Y + 35, buf, C_ACCENT, C_WIN_BG);

    /* draw hint */
    gfx_text_center(WIN_X, WIN_W, CONTENT_Y + 60,
                    "+/- to change  Q to quit", C_TEXT_DIM, C_WIN_BG);
}

int main(void) {
    gfx_init();
    sys_fb_claim();

    gfx_fill(0, 0, gfx_width(), gfx_height(), C_DESKTOP);
    draw_window_chrome();
    redraw_counter();

    last_tick = gfx_ticks();

    for (;;) {
        /* check for auto-increment once per second (100 ticks) */
        long now = gfx_ticks();
        if (now - last_tick >= 100) {
            last_tick = now;
            counter++;
            redraw_counter();
        }

        /* handle input without blocking long */
        unsigned long long raw = sys_key_poll();
        if (raw != 0) {
            key_event_t ev = key_event_unpack(raw);
            if (ev.is_press) {
                if (ev.keycode == KEY_EQUAL || ev.keycode == KEY_KP_PLUS) {
                    counter++;
                    redraw_counter();
                } else if (ev.keycode == KEY_MINUS || ev.keycode == KEY_KP_MINUS) {
                    counter--;
                    redraw_counter();
                } else if (ev.keycode == KEY_Q) {
                    break;
                }
            }
        }

        sys_sleep(2);   /* ~20 ms — keeps CPU load low */
    }

    sys_exit(0);
    return 0;
}
```

**Register in `userspace/CMakeLists.txt`:**
```cmake
add_executable(user_counter
    apps/counter/main.c
    lib/crt0.S
)
target_include_directories(user_counter PRIVATE lib/include)
target_compile_options(user_counter PRIVATE ${USER_COMPILE_FLAGS})
target_link_libraries(user_counter PRIVATE libaether)
target_link_options(user_counter PRIVATE
    -T ${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--build-id=none
)
set_target_properties(user_counter PROPERTIES
    OUTPUT_NAME "counter"
    SUFFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/initrd_staging
)
```

Add `user_counter` to the initrd `DEPENDS` list as shown in [section 3.3](#33-register-the-app-in-the-build-system), then run `ninja` and launch with `counter` from the terminal.

---

## 11. Launching Your App from the Desktop

AetherOS does not have a graphical app launcher yet. There are two ways to run your app:

### From aether_term (the built-in terminal)

Type the app name (without path) at the shell prompt:
```
> hello
```
The terminal spawns the app with `sys_spawn("/hello")` and waits for it to exit.

### Auto-launch from init

To have your app start automatically when the OS boots, edit `userspace/apps/init/main.c` and add a `sys_spawn` call in `main()`:

```c
/* in init/main.c, after existing spawns */
long hello_pid = sys_spawn("/hello");
(void)hello_pid;   /* init does not wait for GUI apps */
```

Rebuild with `ninja`.

### Running on real hardware (Raspberry Pi 4/5)

Copy `build/kernel8.img` to an SD card's FAT32 boot partition (alongside the standard Pi firmware files). The AetherOS kernel boots directly without a bootloader.

---

## 12. Troubleshooting

**App does not appear in QEMU**
- Check that you added `user_YOUR_APP` to the initrd `DEPENDS` list and ran `ninja` after.
- Verify the binary exists: `ls build/initrd_staging/YOUR_APP`

**Screen goes blank or shows garbage**
- Make sure you called `gfx_init()` before any drawing calls.
- Call `sys_fb_claim()` to disable the kernel console.
- The first thing you draw should be a full-screen `gfx_fill(0, 0, 1024, 768, C_DESKTOP)` to clear leftover pixels.

**App crashes / hangs immediately**
- A NULL pointer dereference in userspace triggers an EL0 synchronous exception. The kernel logs a fault message to UART (`serial stdio` in QEMU). Check the terminal output.
- Stack overflow from deep recursion will corrupt adjacent memory silently. Keep recursion depth low.

**`malloc` returns NULL**
- The per-process heap is limited. Avoid large heap allocations; prefer stack buffers for small temporary data.

**Linker error: undefined reference to `__stack_chk_fail`**
- Your compiler flags must include `-fno-stack-protector`. Confirm `USER_COMPILE_FLAGS` is applied via `target_compile_options`.

**Key events feel laggy**
- If using `sys_key_poll` + `sys_sleep`, reduce the sleep interval (try `sys_sleep(1)`).
- If using `sys_key_read` (blocking), ensure you are not performing expensive rendering inside the event handler that stalls re-entering the read.

---

*This guide reflects AetherOS Phase 4.5. The API will evolve as new phases are implemented.*
