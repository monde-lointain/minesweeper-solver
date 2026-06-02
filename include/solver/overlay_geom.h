/* overlay_geom.h — pure overlay geometry helper (Stream A.0).
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

/* Edge rects of a square border ("frame") inscribed in `cell`, `inset` scaled
 * px from each side, border thickness `t` scaled px. Fills out[0..3] = top,
 * bottom, left, right (left/right span the full height, so they also cover the
 * corners). All-integer geometry: drawn as filled rects this is pixel-crisp at
 * EVERY scale. The renderer is 1 logical px = 1 physical px on this machine's
 * monitors (per-monitor-DPI-aware; see overlay.cc), so integer rects align
 * exactly -- unlike an anti-aliased centered stroke, whose even thickness
 * straddles half-pixels and looks fuzzy at odd scales. */
void overlay_box_edges(struct OverlayRect cell, int inset, int t,
                       struct OverlayRect out[4]);

#endif /* SOLVER_OVERLAY_GEOM_H */
