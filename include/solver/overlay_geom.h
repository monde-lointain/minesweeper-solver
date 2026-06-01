/* overlay_geom.h — pure overlay geometry helper. FROZEN CONTRACT (Stream A.0).
 *
 * NO SDL/ImGui — cell-rect geometry only, so it can be unit-tested without a
 * window. Orthodox C++: POD returned by value, pointers.
 */
#ifndef SOLVER_OVERLAY_GEOM_H
#define SOLVER_OVERLAY_GEOM_H

/* Pixel rect of a cell (mirrors render_frame: grid_x + x*cell, ...). */
struct OverlayRect {
  int x;
  int y;
  int w;
  int h;
};

/* Cell (x,y) -> pixel rect. `cell` is the scaled tile size (BLOCK_PX*scale);
 * `grid_x`/`grid_y` are the scaled grid origin from struct Layout. */
struct OverlayRect overlay_cell_rect(int grid_x, int grid_y, int cell, int x,
                                     int y);

#endif /* SOLVER_OVERLAY_GEOM_H */
