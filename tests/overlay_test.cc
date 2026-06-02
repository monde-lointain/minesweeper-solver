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

TEST(OverlayGeom, BoxEdgesCrispAndFlushAtEveryScale) {
  /* The recommended-cell highlight must be a perfect, symmetric, integer-pixel
   * square whose OUTER edge sits exactly on the cell border (inset 0) at any
   * zoom. The renderer is 1:1 on this machine's monitors, so integer rects
   * render crisp; this guards the geometry (regression: an AA centered stroke
   * straddled half-pixels and looked fuzzy at odd scales). */
  for (int scale = 1; scale <= 4; ++scale) {
    int cellpx = 16 * scale;
    int gap = 0;       /* matches overlay.cc: outer edge flush to cell border */
    int t = scale + 1; /* matches overlay.cc */
    struct OverlayRect cell =
        overlay_cell_rect(12 * scale, 12 * scale, cellpx, 0, 0);
    struct OverlayRect e[4];
    overlay_box_edges(cell, gap, t, e);

    /* All four edges share the border thickness. */
    EXPECT_EQ(e[0].h, t) << "scale " << scale; /* top    */
    EXPECT_EQ(e[1].h, t) << "scale " << scale; /* bottom */
    EXPECT_EQ(e[2].w, t) << "scale " << scale; /* left   */
    EXPECT_EQ(e[3].w, t) << "scale " << scale; /* right  */

    /* Outer edge flush to the cell border on all four sides. */
    EXPECT_EQ(e[0].y, cell.y);                       /* top    */
    EXPECT_EQ(e[1].y + e[1].h, cell.y + cell.h);     /* bottom */
    EXPECT_EQ(e[2].x, cell.x);                       /* left   */
    EXPECT_EQ(e[3].x + e[3].w, cell.x + cell.w);     /* right  */

    /* Square: full-cell outer span, equal horizontally and vertically. */
    EXPECT_EQ(e[0].w, cell.w);
    EXPECT_EQ(e[2].h, cell.h);
    EXPECT_EQ(e[0].w, e[2].h);
  }
}
