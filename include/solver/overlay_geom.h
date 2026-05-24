/* overlay_geom.h — pure overlay helpers. FROZEN CONTRACT (Stream A.0).
 *
 * NO SDL/ImGui — geometry, color, and eval-string formatting only, so they can
 * be unit-tested without a window. Orthodox C++: POD returned by value,
 * pointers.
 */
#ifndef SOLVER_OVERLAY_GEOM_H
#define SOLVER_OVERLAY_GEOM_H

#include "solver/engine.h"

/* Pixel rect of a cell (mirrors render_frame: grid_x + x*cell, ...). */
struct OverlayRect {
  int x;
  int y;
  int w;
  int h;
};

/* RGBA tint for a probability overlay. */
struct OverlayColor {
  unsigned char r;
  unsigned char g;
  unsigned char b;
  unsigned char a;
};

/* Cell (x,y) -> pixel rect. `cell` is the scaled tile size (BLOCK_PX*scale);
 * `grid_x`/`grid_y` are the scaled grid origin from struct Layout. */
struct OverlayRect overlay_cell_rect(int grid_x, int grid_y, int cell, int x,
                                     int y);

/* Linear green->yellow->red ramp keyed to P(mine) in [0,1]. */
struct OverlayColor overlay_prob_color(double prob);

/* Format the eval readout (e.g. "SAFE  best (4,2) 98%") into buf[0..n). */
void overlay_eval_string(const struct Analysis* a, char* buf, int n);

#endif /* SOLVER_OVERLAY_GEOM_H */
