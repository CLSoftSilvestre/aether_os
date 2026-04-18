#ifndef AETHER_KEYCODES_H
#define AETHER_KEYCODES_H

/*
 * AetherOS keycode definitions.
 * Used by the PL050 keyboard driver (kernel) and exposed to userspace
 * via userspace/lib/include/input.h (identical definitions).
 *
 * Packed key_event_t (u64):
 *   [63:32] = keycode (keycode_t)
 *   [15:8]  = modifiers bitmask
 *   [0]     = 1 if key press, 0 if key release
 *
 * Packed mouse_event_t (u64):
 *   [63:48] = x (absolute, 0–1023)
 *   [47:32] = y (absolute, 0–767)
 *   [2]     = right button
 *   [1]     = middle button
 *   [0]     = left button
 */

typedef enum {
    KEY_NONE = 0,
    /* Alphabetic */
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
    KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
    KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
    KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    /* Numeric row */
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
    KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    /* Function keys */
    KEY_F1, KEY_F2, KEY_F3,  KEY_F4,
    KEY_F5, KEY_F6, KEY_F7,  KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* Control keys */
    KEY_ENTER, KEY_BACKSPACE, KEY_TAB, KEY_ESC, KEY_SPACE,
    /* Navigation */
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_INSERT, KEY_DELETE,
    /* Punctuation / symbols */
    KEY_MINUS,      /* - / _ */
    KEY_EQUALS,     /* = / + */
    KEY_LBRACKET,   /* [ / { */
    KEY_RBRACKET,   /* ] / } */
    KEY_BACKSLASH,  /* \ / | */
    KEY_SEMICOLON,  /* ; / : */
    KEY_APOSTROPHE, /* ' / " */
    KEY_COMMA,      /* , / < */
    KEY_DOT,        /* . / > */
    KEY_SLASH,      /* / / ? */
    KEY_GRAVE,      /* ` / ~ */
    /* Modifier keys (also reported as press/release events) */
    KEY_LSHIFT, KEY_RSHIFT,
    KEY_LCTRL,  KEY_RCTRL,
    KEY_LALT,   KEY_RALT,
    KEY_CAPS_LOCK,

    KEY_MAX
} keycode_t;

/* Modifier bitmask (packed into [15:8] of key_event_t) */
#define MOD_SHIFT   (1u << 0)
#define MOD_CTRL    (1u << 1)
#define MOD_ALT     (1u << 2)
#define MOD_CAPS    (1u << 3)

/* ── Event structs ─────────────────────────────────────────────────────── */

typedef struct {
    keycode_t    keycode;
    unsigned int modifiers; /* MOD_* bitmask */
    int          is_press;  /* 1 = press, 0 = release */
} key_event_t;

typedef struct {
    unsigned int x;       /* absolute, 0–1023 */
    unsigned int y;       /* absolute, 0–767  */
    unsigned int buttons; /* bit0=left, bit1=middle, bit2=right */
} mouse_event_t;

/* ── Packing helpers (kernel and userspace share these) ─────────────────── */

static inline unsigned long long key_event_pack(key_event_t e)
{
    return ((unsigned long long)(unsigned int)e.keycode << 32) |
           ((unsigned long long)(e.modifiers & 0xFF) << 8) |
           (unsigned long long)(e.is_press & 1);
}

static inline key_event_t key_event_unpack(unsigned long long v)
{
    key_event_t e;
    e.keycode   = (keycode_t)(unsigned int)(v >> 32);
    e.modifiers = (unsigned int)((v >> 8) & 0xFF);
    e.is_press  = (int)(v & 1);
    return e;
}

static inline unsigned long long mouse_event_pack(mouse_event_t e)
{
    return ((unsigned long long)(e.x & 0xFFFFu) << 48) |
           ((unsigned long long)(e.y & 0xFFFFu) << 32) |
           ((unsigned long long)(e.buttons & 0x7));
}

static inline mouse_event_t mouse_event_unpack(unsigned long long v)
{
    mouse_event_t e;
    e.x       = (unsigned int)((v >> 48) & 0xFFFF);
    e.y       = (unsigned int)((v >> 32) & 0xFFFF);
    e.buttons = (unsigned int)(v & 0x7);
    return e;
}

#endif /* AETHER_KEYCODES_H */
