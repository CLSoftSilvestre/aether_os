/* Internal primitive helpers — not part of the public awgt.h API */
#ifndef AWGT_PRIMS_H
#define AWGT_PRIMS_H

void awgt_fill_circle(int cx, int cy, int r, unsigned color);
void awgt_circle(int cx, int cy, int r, unsigned color);
void awgt_line(int x0, int y0, int x1, int y1, unsigned color);
void awgt_line_thick(int x0, int y0, int x1, int y1, int thick, unsigned color);

#endif
