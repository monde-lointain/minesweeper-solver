/* overlay_geom.cc — pure overlay geometry helper (Stream C). No SDL/ImGui.
 *
 * Orthodox C++: POD by value, C headers, C-style casts, index loops.
 */
#include "solver/overlay_geom.h"

struct OverlayRect overlay_cell_rect(int grid_x, int grid_y, int cell, int x,
                                     int y) {
  struct OverlayRect r;
  r.x = grid_x + (x * cell);
  r.y = grid_y + (y * cell);
  r.w = cell;
  r.h = cell;
  return r;
}

void overlay_box_edges(struct OverlayRect cell, int inset, int t,
                       struct OverlayRect out[4]) {
  int x0 = cell.x + inset;
  int y0 = cell.y + inset;
  int w = cell.w - 2 * inset; /* outer frame width  */
  int h = cell.h - 2 * inset; /* outer frame height */

  out[0].x = x0; /* top */
  out[0].y = y0;
  out[0].w = w;
  out[0].h = t;

  out[1].x = x0; /* bottom */
  out[1].y = y0 + h - t;
  out[1].w = w;
  out[1].h = t;

  out[2].x = x0; /* left (full height -> covers the corners) */
  out[2].y = y0;
  out[2].w = t;
  out[2].h = h;

  out[3].x = x0 + w - t; /* right */
  out[3].y = y0;
  out[3].w = t;
  out[3].h = h;
}
