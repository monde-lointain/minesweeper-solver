/* overlay_test.cc — pure overlay-helper tests (Stream C). No SDL. */
#include <gtest/gtest.h>

#include "solver/overlay_geom.h"

TEST(OverlayGeom, CellRectMirrorsRender) {
  /* render_frame draws at grid_x + x*cell, grid_y + y*cell, cell square. */
  struct OverlayRect r = overlay_cell_rect(12, 55, 32, 3, 2);
  EXPECT_EQ(r.x, 12 + 3 * 32);
  EXPECT_EQ(r.y, 55 + 2 * 32);
  EXPECT_EQ(r.w, 32);
  EXPECT_EQ(r.h, 32);

  struct OverlayRect o = overlay_cell_rect(24, 110, 16, 0, 0);
  EXPECT_EQ(o.x, 24);
  EXPECT_EQ(o.y, 110);
  EXPECT_EQ(o.w, 16);
}
