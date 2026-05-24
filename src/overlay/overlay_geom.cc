/* overlay_geom.cc — pure overlay helpers. STUB (Stream A.0); Stream C implements. */
#include "solver/overlay_geom.h"

#include <stdio.h>

struct OverlayRect overlay_cell_rect(int grid_x, int grid_y, int cell, int x,
                                     int y) {
  struct OverlayRect r;
  r.x = grid_x + x * cell;
  r.y = grid_y + y * cell;
  r.w = cell;
  r.h = cell;
  return r;
}

struct OverlayColor overlay_prob_color(double prob) {
  struct OverlayColor c;
  (void)prob;
  c.r = 0;
  c.g = 0;
  c.b = 0;
  c.a = 0;
  return c;
}

void overlay_eval_string(const struct Analysis *a, char *buf, int n) {
  (void)a;
  if (n > 0) {
    buf[0] = '\0';
  }
}
