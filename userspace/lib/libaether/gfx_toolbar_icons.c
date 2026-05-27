/*
 * AetherOS — Toolbar icon renderer
 * File: userspace/lib/libaether/gfx_toolbar_icons.c
 *
 * Each icon is drawn into a 14×14 pixel cell at (x, y) using gfx_fill /
 * gfx_hline / gfx_vline — no external image data required.
 *
 * Icons are designed for Lumina accent-coloured (purple) toolbar buttons.
 * Primary shapes use off-white; accent colours (green, gold, red) mark
 * action-specific icons so they are distinguishable at a glance.
 */

#include <gfx.h>

/* ── Shared palette ──────────────────────────────────────────────────────── */
#define IC_WHITE   GFX_RGB(232, 232, 245)   /* page / arrow body */
#define IC_RULE    GFX_RGB(130, 130, 155)   /* rule lines, shadow details */
#define IC_METAL   GFX_RGB(155, 158, 178)   /* floppy metal body */
#define IC_LABEL   GFX_RGB(228, 228, 238)   /* floppy label area */
#define IC_SLOT    GFX_RGB( 22,  22,  42)   /* floppy read/write slot */
#define IC_GOLD    GFX_RGB(240, 190,  50)   /* folder gold */
#define IC_GDARK   GFX_RGB(185, 140,  30)   /* folder shadow */
#define IC_GREEN   GFX_RGB( 70, 200,  80)   /* play / run */
#define IC_RED     GFX_RGB(215,  58,  58)   /* stop / clear */
#define IC_TRASH   GFX_RGB(190, 190, 210)   /* trash can body */
#define IC_TDARK   GFX_RGB( 38,  38,  60)   /* trash stripes */
#define IC_ARROW   GFX_RGB(200, 200, 220)   /* undo / redo */
#define IC_SCIS    GFX_RGB(210, 210, 228)   /* scissors */
#define IC_COPY    GFX_RGB(218, 218, 235)   /* copy pages */
#define IC_CLIP    GFX_RGB(200, 200, 218)   /* clipboard */
#define IC_CBAR    GFX_RGB(245, 185,  55)   /* clipboard bar */

/* ── Individual icon renderers ───────────────────────────────────────────── */

/* Blank document with dog-ear fold at top-right */
static void icon_new(int x, int y)
{
    gfx_fill(x,    y,    10, 14, IC_WHITE);   /* page body */
    gfx_fill(x+10, y+3,   4, 11, IC_WHITE);  /* right strip below fold */
    /* fold crease — highlight where the corner bends */
    gfx_hline(x+10, y+3, 4,    IC_RULE);
    gfx_vline(x+10, y,   3,    IC_RULE);
    /* rule lines */
    gfx_fill(x+2, y+5,  7, 1, IC_RULE);
    gfx_fill(x+2, y+8,  7, 1, IC_RULE);
    gfx_fill(x+2, y+11, 5, 1, IC_RULE);
}

/* Open folder — tab + body + shadow lines */
static void icon_open(int x, int y)
{
    gfx_fill(x,   y+3, 5,  2, IC_GOLD);   /* folder tab */
    gfx_fill(x,   y+5, 14, 9, IC_GOLD);   /* folder body */
    gfx_fill(x+5, y+3,  9, 2, IC_GOLD);   /* body top to meet tab */
    gfx_fill(x,   y+5, 14, 2, GFX_RGB(255, 210, 90)); /* lighter top edge */
    /* shadow lines inside */
    gfx_fill(x+2, y+8,  10, 1, IC_GDARK);
    gfx_fill(x+2, y+11, 10, 1, IC_GDARK);
}

/* Floppy disk — metal body, label, read/write window */
static void icon_save(int x, int y)
{
    gfx_fill(x,   y,   14, 14, IC_METAL);  /* metal shell */
    gfx_fill(x+1, y+1,  9,  5, IC_LABEL);  /* label area */
    /* metal slider button on top-right */
    gfx_fill(x+11, y+1, 2,  4, IC_METAL);
    /* read/write window at bottom */
    gfx_fill(x+2, y+9, 10,  4, IC_SLOT);
    gfx_fill(x+4, y+11, 6,  1, IC_METAL);  /* centre bar in window */
}

/* Right-pointing play triangle */
static void icon_run(int x, int y)
{
    gfx_fill(x+1,  y+2,  2, 10, IC_GREEN);
    gfx_fill(x+3,  y+3,  2,  8, IC_GREEN);
    gfx_fill(x+5,  y+4,  2,  6, IC_GREEN);
    gfx_fill(x+7,  y+5,  2,  4, IC_GREEN);
    gfx_fill(x+9,  y+6,  2,  2, IC_GREEN);
}

/* Filled square — stop / halt */
static void icon_stop(int x, int y)
{
    gfx_fill(x+2, y+2, 10, 10, IC_RED);
}

/* Trash can — lid + handle + body + stripes */
static void icon_clear(int x, int y)
{
    /* handle */
    gfx_fill(x+5, y,    4, 2, IC_TRASH);
    /* lid */
    gfx_fill(x+1, y+2, 12, 2, IC_TRASH);
    /* body */
    gfx_fill(x+2, y+4, 10, 9, IC_TRASH);
    /* stripes */
    gfx_fill(x+4, y+6,  2, 6, IC_TDARK);
    gfx_fill(x+7, y+6,  2, 6, IC_TDARK);
    gfx_fill(x+10,y+6,  1, 6, IC_TDARK);
}

/* Counter-clockwise arc arrow (undo) */
static void icon_undo(int x, int y)
{
    /* arc: top bar */
    gfx_fill(x+3, y+1,  7, 2, IC_ARROW);
    /* arc: right side going down */
    gfx_fill(x+9, y+3,  2, 5, IC_ARROW);
    /* horizontal shaft back to left */
    gfx_fill(x+3, y+8,  7, 2, IC_ARROW);
    /* arrowhead — left-pointing triangle */
    gfx_fill(x+1, y+7,  2, 4, IC_ARROW);  /* outer edge */
    gfx_fill(x+2, y+8,  1, 2, IC_ARROW);  /* tip highlight */
}

/* Clockwise arc arrow (redo) — mirror of undo */
static void icon_redo(int x, int y)
{
    /* arc: top bar */
    gfx_fill(x+4, y+1,  7, 2, IC_ARROW);
    /* arc: left side going down */
    gfx_fill(x+3, y+3,  2, 5, IC_ARROW);
    /* horizontal shaft back to right */
    gfx_fill(x+4, y+8,  7, 2, IC_ARROW);
    /* arrowhead — right-pointing triangle */
    gfx_fill(x+11, y+7, 2, 4, IC_ARROW);  /* outer edge */
    gfx_fill(x+11, y+8, 1, 2, IC_ARROW);  /* tip highlight */
}

/* Scissors — two angled blades meeting at a pivot */
static void icon_cut(int x, int y)
{
    /* upper blade (angled top-left to bottom-right) */
    gfx_fill(x+1, y+1, 2, 2, IC_SCIS);   /* handle ring */
    gfx_fill(x+3, y+3, 2, 2, IC_SCIS);
    gfx_fill(x+5, y+5, 2, 2, IC_SCIS);
    gfx_fill(x+7, y+4, 4, 2, IC_SCIS);   /* blade tip upper */
    /* lower blade (angled bottom-left to top-right) */
    gfx_fill(x+1, y+11, 2, 2, IC_SCIS);  /* handle ring */
    gfx_fill(x+3, y+9,  2, 2, IC_SCIS);
    gfx_fill(x+5, y+7,  2, 2, IC_SCIS);  /* meet at pivot */
    gfx_fill(x+7, y+8,  4, 2, IC_SCIS);  /* blade tip lower */
    /* pivot dot */
    gfx_fill(x+5, y+6, 2, 2, IC_SCIS);
}

/* Two overlapping pages (copy) */
static void icon_copy(int x, int y)
{
    /* back page (slightly offset right-down) */
    gfx_fill(x+4, y+3,  9, 11, IC_RULE);   /* back page outline */
    gfx_fill(x+5, y+4,  7,  9, IC_COPY);   /* back page fill */
    /* front page */
    gfx_fill(x,   y,   10, 11, IC_COPY);
    /* front page dog-ear */
    gfx_fill(x+8, y,    2,  2, IC_RULE);   /* fold */
    gfx_fill(x+8, y+2,  2,  1, IC_COPY);   /* crease */
    /* rule lines on front page */
    gfx_fill(x+2, y+4,  5, 1, IC_RULE);
    gfx_fill(x+2, y+6,  5, 1, IC_RULE);
    gfx_fill(x+2, y+8,  4, 1, IC_RULE);
}

/* Clipboard — board + clip bar + lines */
static void icon_paste(int x, int y)
{
    /* clipboard board */
    gfx_fill(x+1, y+2, 12, 12, IC_CLIP);
    /* clip bar at top */
    gfx_fill(x+4, y,    6,  4, IC_CBAR);
    gfx_fill(x+5, y+1,  4,  2, IC_CLIP);  /* clip hole */
    /* rule lines on board */
    gfx_fill(x+3, y+6,  8, 1, IC_RULE);
    gfx_fill(x+3, y+9,  8, 1, IC_RULE);
    gfx_fill(x+3, y+12, 6, 1, IC_RULE);
}

/* Envelope body outline (shared shape for mail icons, 14×14 cell) */
static void draw_envelope_body(int x, int y)
{
    /* Envelope body: off-white rectangle */
    gfx_fill(x+1, y+4, 12, 9, IC_WHITE);
    /* Border */
    gfx_hline(x+1, y+4,  12, IC_RULE);   /* top edge */
    gfx_hline(x+1, y+12, 12, IC_RULE);   /* bottom edge */
    gfx_vline(x+1, y+4,   9, IC_RULE);   /* left edge */
    gfx_vline(x+12, y+4,  9, IC_RULE);   /* right edge */
    /* V-flap lines from corners to centre */
    gfx_fill(x+2,  y+5, 4, 1, IC_RULE);
    gfx_fill(x+3,  y+6, 2, 1, IC_RULE);
    gfx_fill(x+4,  y+7, 2, 1, IC_RULE);  /* left diagonal */
    gfx_fill(x+8,  y+7, 2, 1, IC_RULE);
    gfx_fill(x+9,  y+6, 2, 1, IC_RULE);
    gfx_fill(x+10, y+5, 2, 1, IC_RULE);  /* right diagonal */
    gfx_fill(x+6,  y+8, 2, 1, IC_RULE);  /* centre V tip */
}

/* Envelope + pen nib (compose) */
static void icon_compose(int x, int y)
{
    draw_envelope_body(x, y);
    /* Pen nib at top-right: diagonal stroke */
    gfx_fill(x+11, y+1, 2, 2, IC_GREEN);   /* pen tip */
    gfx_fill(x+10, y+2, 1, 1, IC_GREEN);
    gfx_fill(x+9,  y+3, 1, 1, IC_GREEN);
}

/* Envelope + left-pointing arrow (reply) */
static void icon_reply(int x, int y)
{
    draw_envelope_body(x, y);
    /* Left arrow above envelope */
    gfx_fill(x+1, y+2, 5, 1, IC_ARROW);    /* shaft */
    gfx_fill(x+1, y+1, 1, 1, IC_ARROW);    /* arrowhead top */
    gfx_fill(x+1, y+3, 1, 1, IC_ARROW);    /* arrowhead bottom */
    gfx_fill(x+2, y+2, 1, 1, IC_ARROW);    /* tip highlight */
}

/* Envelope + right-pointing arrow (forward) */
static void icon_forward_mail(int x, int y)
{
    draw_envelope_body(x, y);
    /* Right arrow above envelope */
    gfx_fill(x+7, y+2, 5, 1, IC_ARROW);    /* shaft */
    gfx_fill(x+11, y+1, 1, 1, IC_ARROW);   /* arrowhead top */
    gfx_fill(x+11, y+3, 1, 1, IC_ARROW);   /* arrowhead bottom */
    gfx_fill(x+11, y+2, 1, 1, IC_ARROW);   /* tip */
}

/* Circular refresh arrow (clockwise loop) */
static void icon_refresh(int x, int y)
{
    /* Arc top: horizontal bar */
    gfx_fill(x+3, y+1,  7, 2, IC_ARROW);
    /* Arc right: vertical bar */
    gfx_fill(x+9, y+3,  2, 5, IC_ARROW);
    /* Arc bottom: horizontal bar */
    gfx_fill(x+3, y+10, 7, 2, IC_ARROW);
    /* Arc left: vertical bar */
    gfx_fill(x+2, y+3,  2, 5, IC_ARROW);
    /* Arrowhead at top-right (clockwise) */
    gfx_fill(x+10, y+1, 2, 2, IC_ARROW);   /* tip block */
    gfx_fill(x+10, y+1, 1, 3, IC_ARROW);
}

/* 4-tooth gear (account settings) */
static void icon_settings(int x, int y)
{
    unsigned c = IC_WHITE;
    gfx_fill((unsigned)(x+3),  (unsigned)(y+3),  8u, 8u, c); /* body */
    gfx_fill((unsigned)(x+5),  (unsigned)(y+0),  4u, 3u, c); /* top tooth */
    gfx_fill((unsigned)(x+5),  (unsigned)(y+11), 4u, 3u, c); /* bottom tooth */
    gfx_fill((unsigned)(x+0),  (unsigned)(y+5),  4u, 4u, c); /* left tooth */
    gfx_fill((unsigned)(x+10), (unsigned)(y+5),  4u, 4u, c); /* right tooth */
}

/* Left-pointing navigation arrow (browser back) */
static void icon_nav_back(int x, int y)
{
    gfx_fill(x+6, y+5, 7, 4, IC_ARROW);  /* shaft */
    gfx_fill(x+5, y+4, 1, 6, IC_ARROW);  /* arrowhead outer wing */
    gfx_fill(x+3, y+5, 2, 4, IC_ARROW);  /* arrowhead middle */
    gfx_fill(x+1, y+6, 2, 2, IC_ARROW);  /* arrowhead inner */
    gfx_fill(x+0, y+7, 1, 1, IC_ARROW);  /* tip */
}

/* Right-pointing navigation arrow (browser forward) */
static void icon_nav_fwd(int x, int y)
{
    gfx_fill(x+1, y+5, 7, 4, IC_ARROW);   /* shaft */
    gfx_fill(x+8, y+4, 1, 6, IC_ARROW);   /* arrowhead outer wing */
    gfx_fill(x+9, y+5, 2, 4, IC_ARROW);   /* arrowhead middle */
    gfx_fill(x+11, y+6, 2, 2, IC_ARROW);  /* arrowhead inner */
    gfx_fill(x+13, y+7, 1, 1, IC_ARROW);  /* tip */
}

/* 5-pointed star shape shared by outline and filled variants */
static void icon_star_draw(int x, int y, unsigned c)
{
    /* Top spike */
    gfx_fill(x+5, y+0, 4, 3, c);
    /* Arm band */
    gfx_fill(x+0, y+3, 14, 3, c);
    /* Upper body */
    gfx_fill(x+2, y+6, 10, 2, c);
    /* Lower left spike */
    gfx_fill(x+0, y+8,  4, 5, c);
    /* Lower right spike */
    gfx_fill(x+10, y+8, 4, 5, c);
    /* Lower center connector (V between spikes) */
    gfx_fill(x+5, y+8,  4, 3, c);
}

/* Star outline — not bookmarked */
static void icon_star(int x, int y)
{
    icon_star_draw(x, y, GFX_RGB(100, 95, 55));
}

/* Filled star — bookmarked */
static void icon_star_filled(int x, int y)
{
    icon_star_draw(x, y, GFX_RGB(247, 201, 72));
}

/* Plus sign — new tab */
static void icon_newtab(int x, int y)
{
    gfx_fill(x+6, y+1,  2, 12, IC_ARROW);  /* vertical bar */
    gfx_fill(x+1, y+6, 12,  2, IC_ARROW);  /* horizontal bar */
}

/* Small X — close tab */
static void icon_close_small(int x, int y)
{
    unsigned c = IC_RED;
    gfx_fill(x+2,  y+2,  2, 2, c);
    gfx_fill(x+10, y+2,  2, 2, c);
    gfx_fill(x+4,  y+4,  2, 2, c);
    gfx_fill(x+8,  y+4,  2, 2, c);
    gfx_fill(x+6,  y+6,  2, 2, c);  /* centre */
    gfx_fill(x+4,  y+8,  2, 2, c);
    gfx_fill(x+8,  y+8,  2, 2, c);
    gfx_fill(x+2,  y+10, 2, 2, c);
    gfx_fill(x+10, y+10, 2, 2, c);
}

/* ── Dispatch ─────────────────────────────────────────────────────────────── */

void gfx_toolbar_icon(int x, int y, unsigned char icon_id)
{
    switch (icon_id) {
    case ICON_BTN_NEW:     icon_new(x, y);          break;
    case ICON_BTN_OPEN:    icon_open(x, y);         break;
    case ICON_BTN_SAVE:    icon_save(x, y);         break;
    case ICON_BTN_RUN:     icon_run(x, y);          break;
    case ICON_BTN_STOP:    icon_stop(x, y);         break;
    case ICON_BTN_CLEAR:   icon_clear(x, y);        break;
    case ICON_BTN_UNDO:    icon_undo(x, y);         break;
    case ICON_BTN_REDO:    icon_redo(x, y);         break;
    case ICON_BTN_CUT:     icon_cut(x, y);          break;
    case ICON_BTN_COPY:    icon_copy(x, y);         break;
    case ICON_BTN_PASTE:   icon_paste(x, y);        break;
    case ICON_BTN_COMPOSE: icon_compose(x, y);      break;
    case ICON_BTN_REPLY:   icon_reply(x, y);        break;
    case ICON_BTN_FORWARD: icon_forward_mail(x, y); break;
    case ICON_BTN_REFRESH:      icon_refresh(x, y);      break;
    case ICON_BTN_SETTINGS:     icon_settings(x, y);     break;
    case ICON_BTN_NAV_BACK:     icon_nav_back(x, y);     break;
    case ICON_BTN_NAV_FWD:      icon_nav_fwd(x, y);      break;
    case ICON_BTN_STAR:         icon_star(x, y);         break;
    case ICON_BTN_STAR_FILLED:  icon_star_filled(x, y);  break;
    case ICON_BTN_NEWTAB:       icon_newtab(x, y);       break;
    case ICON_BTN_CLOSE_SMALL:  icon_close_small(x, y);  break;
    default: break;
    }
}
