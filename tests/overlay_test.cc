/* overlay_test.cc — STUB (Stream A.0); Stream C writes the real suite. */
#include <gtest/gtest.h>

#include "solver/overlay_geom.h"

TEST(OverlayGeomStub, CellRectOrigin) {
  struct OverlayRect r = overlay_cell_rect(12, 55, 32, 0, 0);
  EXPECT_EQ(r.x, 12);
  EXPECT_EQ(r.y, 55);
  EXPECT_EQ(r.w, 32);
  EXPECT_EQ(r.h, 32);
}
