#ifndef AETHER_CURSOR_H
#define AETHER_CURSOR_H

/*
 * Software cursor overlay.
 *
 * The kernel maintains a 16×16 arrow sprite.  Before drawing the sprite the
 * pixels underneath are saved; they are restored before the cursor moves.
 *
 * cursor_init()  — called once after the framebuffer is ready
 * cursor_show()  — make cursor visible (shows at current position)
 * cursor_hide()  — erase cursor (restore saved background)
 * cursor_move()  — atomically restore old bg, save new bg, draw at (x, y)
 */

void cursor_init(void);
void cursor_show(void);
void cursor_hide(void);
void cursor_move(unsigned int x, unsigned int y);
void cursor_get_pos(unsigned int *x, unsigned int *y);

#endif /* AETHER_CURSOR_H */
