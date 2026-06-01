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
