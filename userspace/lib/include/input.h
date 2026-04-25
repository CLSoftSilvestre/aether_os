#ifndef AETHER_USERSPACE_INPUT_H
#define AETHER_USERSPACE_INPUT_H

/*
 * AetherOS userspace input types.
 * Mirrors kernel/drivers/input/keycodes.h — keep in sync.
 *
 * key_event_t and mouse_event_t are packed into u64 when passed across
 * the syscall boundary; use the unpack helpers to decode them.
 */

typedef enum {
    KEY_NONE = 0,
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
    KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
    KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
    KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
    KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_F1, KEY_F2, KEY_F3,  KEY_F4,
    KEY_F5, KEY_F6, KEY_F7,  KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_ENTER, KEY_BACKSPACE, KEY_TAB, KEY_ESC, KEY_SPACE,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE,
    KEY_MINUS, KEY_EQUALS,
    KEY_LBRACKET, KEY_RBRACKET,
    KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE,
    KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_GRAVE,
    KEY_LSHIFT, KEY_RSHIFT,
    KEY_LCTRL,  KEY_RCTRL,
    KEY_LALT,   KEY_RALT,
    KEY_CAPS_LOCK,
    KEY_MAX
} keycode_t;

/* Modifier bitmask */
#define MOD_SHIFT   (1u << 0)
#define MOD_CTRL    (1u << 1)
#define MOD_ALT     (1u << 2)
#define MOD_CAPS    (1u << 3)

typedef struct {
    keycode_t    keycode;
    unsigned int modifiers;
    int          is_press;
} key_event_t;

typedef struct {
    unsigned int x;
    unsigned int y;
    unsigned int buttons;   /* bit0=left, bit1=middle, bit2=right */
} mouse_event_t;

/* ── WM event constants ──────────────────────────────────────────────────── */

/*
 * WM_EV_REDRAW — synthetic WM event delivered to a window's PID when init
 * drags the window to a new position (via SYS_WM_MOVE).
 *
 * The event is packed as a u64 with:
 *   [63:32] = WM_EV_REDRAW (0xFE)
 *   [31:16] = new x coordinate
 *   [15:0]  = new y coordinate
 *
 * Check with wm_event_is_redraw() before calling key_event_unpack().
 */
#define WM_EV_REDRAW  0xFEu

static inline int wm_event_is_redraw(unsigned long long v)
{
    return ((unsigned int)(v >> 32)) == WM_EV_REDRAW;
}

static inline int wm_event_redraw_x(unsigned long long v)
{
    return (int)((v >> 16) & 0xFFFFu);
}

static inline int wm_event_redraw_y(unsigned long long v)
{
    return (int)(v & 0xFFFFu);
}

/* ── Unpack helpers ─────────────────────────────────────────────────────── */

static inline key_event_t key_event_unpack(unsigned long long v)
{
    key_event_t e;
    e.keycode   = (keycode_t)(unsigned int)(v >> 32);
    e.modifiers = (unsigned int)((v >> 8) & 0xFF);
    e.is_press  = (int)(v & 1);
    return e;
}

static inline mouse_event_t mouse_event_unpack(unsigned long long v)
{
    mouse_event_t e;
    e.x       = (unsigned int)((v >> 48) & 0xFFFF);
    e.y       = (unsigned int)((v >> 32) & 0xFFFF);
    e.buttons = (unsigned int)(v & 0x7);
    return e;
}

#endif /* AETHER_USERSPACE_INPUT_H */
