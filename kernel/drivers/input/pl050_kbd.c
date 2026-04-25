/*
 * AetherOS — PL050 KMI Keyboard Driver
 * File: kernel/drivers/input/pl050_kbd.c
 *
 * ARM PrimeCell KMI (PL050) PS/2 keyboard controller.
 * QEMU -M virt provides this device at 0x09050000, GIC IRQ 52 (SPI 20).
 *
 * PS/2 Set 2 scan-code state machine:
 *   IDLE       — normal byte
 *   GOT_E0     — extended prefix received (arrow keys, etc.)
 *   GOT_F0     — break (key-release) prefix received
 *   GOT_E0_F0  — extended break
 *
 * Each complete sequence is decoded into a key_event_t and pushed onto a
 * 32-entry circular ring buffer.  Syscall handlers drain the buffer.
 */

#include "drivers/input/pl050_kbd.h"
#include "drivers/input/keycodes.h"
#include "aether/types.h"
#include "aether/printk.h"
#include "drivers/irq/gic_v2.h"

/* ── MMIO register map ──────────────────────────────────────────────────── */

#define KMI_KBD_BASE    0x09050000UL
#define KMI_IRQ_KBD     52

#define KMI_CR_OFF      0x00    /* Control */
#define KMI_STAT_OFF    0x04    /* Status  */
#define KMI_DATA_OFF    0x08    /* Data    */
#define KMI_IR_OFF      0x10    /* IRQ status */

/* Control register bits */
#define KMI_CR_EN       (1u << 2)   /* Enable interface */
#define KMI_CR_RXINTREN (1u << 4)   /* Enable RX interrupt */

/* Status register bits */
#define KMI_STAT_RXFULL (1u << 4)   /* Receive buffer full */

#define REG32(base, off) (*((volatile u32 *)((uintptr_t)(base) + (off))))

/* ── PS/2 Set 2 → keycode translation table ─────────────────────────────── */

/* Normal (no 0xE0 prefix) scan codes → keycode */
static const u8 sc2_normal[256] = {
    [0x1C] = KEY_A, [0x32] = KEY_B, [0x21] = KEY_C, [0x23] = KEY_D,
    [0x24] = KEY_E, [0x2B] = KEY_F, [0x34] = KEY_G, [0x33] = KEY_H,
    [0x43] = KEY_I, [0x3B] = KEY_J, [0x42] = KEY_K, [0x4B] = KEY_L,
    [0x3A] = KEY_M, [0x31] = KEY_N, [0x44] = KEY_O, [0x4D] = KEY_P,
    [0x15] = KEY_Q, [0x2D] = KEY_R, [0x1B] = KEY_S, [0x2C] = KEY_T,
    [0x3C] = KEY_U, [0x2A] = KEY_V, [0x1D] = KEY_W, [0x22] = KEY_X,
    [0x35] = KEY_Y, [0x1A] = KEY_Z,
    /* Digits */
    [0x45] = KEY_0, [0x16] = KEY_1, [0x1E] = KEY_2, [0x26] = KEY_3,
    [0x25] = KEY_4, [0x2E] = KEY_5, [0x36] = KEY_6, [0x3D] = KEY_7,
    [0x3E] = KEY_8, [0x46] = KEY_9,
    /* Function keys */
    [0x05] = KEY_F1,  [0x06] = KEY_F2,  [0x04] = KEY_F3,  [0x0C] = KEY_F4,
    [0x03] = KEY_F5,  [0x0B] = KEY_F6,  [0x83] = KEY_F7,  [0x0A] = KEY_F8,
    [0x01] = KEY_F9,  [0x09] = KEY_F10, [0x78] = KEY_F11, [0x07] = KEY_F12,
    /* Control */
    [0x5A] = KEY_ENTER,     [0x66] = KEY_BACKSPACE,
    [0x0D] = KEY_TAB,       [0x76] = KEY_ESC,
    [0x29] = KEY_SPACE,
    /* Punctuation */
    [0x4E] = KEY_MINUS,     [0x55] = KEY_EQUALS,
    [0x54] = KEY_LBRACKET,  [0x5B] = KEY_RBRACKET,
    [0x5D] = KEY_BACKSLASH, [0x4C] = KEY_SEMICOLON,
    [0x52] = KEY_APOSTROPHE,[0x41] = KEY_COMMA,
    [0x49] = KEY_DOT,       [0x4A] = KEY_SLASH,
    [0x0E] = KEY_GRAVE,
    /* Modifiers */
    [0x12] = KEY_LSHIFT, [0x59] = KEY_RSHIFT,
    [0x14] = KEY_LCTRL,  [0x11] = KEY_LALT,
    [0x58] = KEY_CAPS_LOCK,
};

/* Extended (0xE0 prefix) scan codes → keycode */
static const u8 sc2_extended[256] = {
    [0x75] = KEY_UP,     [0x72] = KEY_DOWN,
    [0x6B] = KEY_LEFT,   [0x74] = KEY_RIGHT,
    [0x6C] = KEY_HOME,   [0x69] = KEY_END,
    [0x7D] = KEY_PGUP,   [0x7A] = KEY_PGDN,
    [0x70] = KEY_INSERT, [0x71] = KEY_DELETE,
    [0x14] = KEY_RCTRL,  [0x11] = KEY_RALT,
};

/* ── Scan-code state machine ─────────────────────────────────────────────── */

typedef enum {
    KBD_IDLE,
    KBD_GOT_E0,
    KBD_GOT_F0,
    KBD_GOT_E0_F0,
} kbd_state_t;

static kbd_state_t kbd_state;
static unsigned int kbd_modifiers;

static void update_modifiers(keycode_t kc, int press)
{
    unsigned int bit = 0;
    switch (kc) {
    case KEY_LSHIFT: case KEY_RSHIFT: bit = MOD_SHIFT; break;
    case KEY_LCTRL:  case KEY_RCTRL:  bit = MOD_CTRL;  break;
    case KEY_LALT:   case KEY_RALT:   bit = MOD_ALT;   break;
    case KEY_CAPS_LOCK:
        if (press) kbd_modifiers ^= MOD_CAPS;
        return;
    default: return;
    }
    if (press) kbd_modifiers |= bit;
    else       kbd_modifiers &= ~bit;
}

/* ── Key event ring buffer ──────────────────────────────────────────────── */

#define KBD_RING_SIZE 32

static unsigned long long kbd_ring[KBD_RING_SIZE];
static volatile unsigned int kbd_head;
static volatile unsigned int kbd_tail;

static void kbd_ring_push(unsigned long long ev)
{
    unsigned int next = (kbd_head + 1) % KBD_RING_SIZE;
    if (next == kbd_tail) return;   /* drop on overflow */
    kbd_ring[kbd_head] = ev;
    kbd_head = next;
}

int kbd_event_empty(void) { return kbd_head == kbd_tail; }

void kbd_push_key(keycode_t kc, int is_press)
{
    update_modifiers(kc, is_press);
    key_event_t ev = { kc, kbd_modifiers, is_press };
    kbd_ring_push(key_event_pack(ev));
}

unsigned long long kbd_get_event(void)
{
    unsigned long long v = kbd_ring[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_RING_SIZE;
    return v;
}

unsigned int kbd_get_modifiers(void) { return kbd_modifiers; }

/* ── Process one PS/2 byte from the IRQ handler ─────────────────────────── */

static void process_byte(u8 byte)
{
    switch (kbd_state) {
    case KBD_IDLE:
        if (byte == 0xE0) { kbd_state = KBD_GOT_E0; return; }
        if (byte == 0xF0) { kbd_state = KBD_GOT_F0; return; }
        /* Normal press */
        {
            keycode_t kc = (keycode_t)sc2_normal[byte];
            if (kc != KEY_NONE) {
                update_modifiers(kc, 1);
                key_event_t ev = { kc, kbd_modifiers, 1 };
                kbd_ring_push(key_event_pack(ev));
            }
        }
        break;

    case KBD_GOT_E0:
        if (byte == 0xF0) { kbd_state = KBD_GOT_E0_F0; return; }
        /* Extended press */
        {
            keycode_t kc = (keycode_t)sc2_extended[byte];
            if (kc != KEY_NONE) {
                update_modifiers(kc, 1);
                key_event_t ev = { kc, kbd_modifiers, 1 };
                kbd_ring_push(key_event_pack(ev));
            }
        }
        kbd_state = KBD_IDLE;
        break;

    case KBD_GOT_F0:
        /* Normal release */
        {
            keycode_t kc = (keycode_t)sc2_normal[byte];
            if (kc != KEY_NONE) {
                update_modifiers(kc, 0);
                key_event_t ev = { kc, kbd_modifiers, 0 };
                kbd_ring_push(key_event_pack(ev));
            }
        }
        kbd_state = KBD_IDLE;
        break;

    case KBD_GOT_E0_F0:
        /* Extended release */
        {
            keycode_t kc = (keycode_t)sc2_extended[byte];
            if (kc != KEY_NONE) {
                update_modifiers(kc, 0);
                key_event_t ev = { kc, kbd_modifiers, 0 };
                kbd_ring_push(key_event_pack(ev));
            }
        }
        kbd_state = KBD_IDLE;
        break;
    }
}

/* ── IRQ handler ────────────────────────────────────────────────────────── */

void pl050_kbd_irq_handler(void)
{
    /* Drain the RX FIFO — there may be more than one byte pending */
    while (REG32(KMI_KBD_BASE, KMI_STAT_OFF) & KMI_STAT_RXFULL) {
        u8 byte = (u8)REG32(KMI_KBD_BASE, KMI_DATA_OFF);
        process_byte(byte);
    }
}

/* ── Initialization ─────────────────────────────────────────────────────── */

void pl050_kbd_init(void)
{
    kbd_state     = KBD_IDLE;
    kbd_modifiers = 0;
    kbd_head      = 0;
    kbd_tail      = 0;

    /* Enable controller + RX interrupt */
    REG32(KMI_KBD_BASE, KMI_CR_OFF) = KMI_CR_EN | KMI_CR_RXINTREN;

    gic_enable_irq(KMI_IRQ_KBD);

    kinfo("KBD: PL050 keyboard init — MMIO 0x%lx, IRQ %u\n",
          (unsigned long)KMI_KBD_BASE, KMI_IRQ_KBD);
}
