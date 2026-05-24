/* overlay_test.cc — pure overlay-helper tests (Stream C). No SDL. */
#include <string.h>

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

TEST(OverlayGeom, ColorEndpoints) {
  struct OverlayColor lo = overlay_prob_color(0.0);
  EXPECT_EQ(lo.r, 60);
  EXPECT_EQ(lo.g, 200);
  EXPECT_EQ(lo.b, 60);
  struct OverlayColor mid = overlay_prob_color(0.5);
  EXPECT_EQ(mid.r, 240);
  EXPECT_EQ(mid.g, 200);
  struct OverlayColor hi = overlay_prob_color(1.0);
  EXPECT_EQ(hi.r, 225);
  EXPECT_EQ(hi.g, 70);
  EXPECT_EQ(lo.a, 148);
}

TEST(OverlayGeom, ColorRampMonotone) {
  /* green channel non-increasing; "redness" (r-g) strictly increasing. */
  double samples[] = {0.0, 0.2, 0.4, 0.5, 0.7, 0.9, 1.0};
  int prev_g = 256;
  int prev_diff = -1000;
  for (double p : samples) {
    struct OverlayColor c = overlay_prob_color(p);
    EXPECT_LE((int)c.g, prev_g);
    int diff = (int)c.r - (int)c.g;
    EXPECT_GT(diff, prev_diff);
    prev_g = (int)c.g;
    prev_diff = diff;
  }
}

TEST(OverlayGeom, ColorClampsOutOfRange) {
  struct OverlayColor lo = overlay_prob_color(-5.0);
  struct OverlayColor hi = overlay_prob_color(5.0);
  EXPECT_EQ(lo.r, overlay_prob_color(0.0).r);
  EXPECT_EQ(hi.r, overlay_prob_color(1.0).r);
}

TEST(OverlayGeom, EvalStringSafe) {
  Analysis a;
  memset(&a, 0, sizeof a);
  a.eval = EVAL_SAFE;
  a.best_x = 4;
  a.best_y = 2;
  a.best_prob = 0.0;
  char buf[128];
  overlay_eval_string(&a, buf, sizeof buf);
  EXPECT_NE(strstr(buf, "SAFE"), nullptr);
  EXPECT_NE(strstr(buf, "(4,2)"), nullptr);
  EXPECT_NE(strstr(buf, "100%"), nullptr);
}

TEST(OverlayGeom, EvalStringGuessWithInterior) {
  Analysis a;
  memset(&a, 0, sizeof a);
  a.eval = EVAL_GUESS;
  a.best_x = 1;
  a.best_y = 0;
  a.best_prob = 0.16; /* 84% safe */
  a.interior_count = 30;
  a.interior_prob = 0.20; /* 80% safe */
  char buf[128];
  overlay_eval_string(&a, buf, sizeof buf);
  EXPECT_NE(strstr(buf, "GUESS"), nullptr);
  EXPECT_NE(strstr(buf, "84%"), nullptr);
  EXPECT_NE(strstr(buf, "interior 80%"), nullptr);
}

TEST(OverlayGeom, EvalStringStartSolvedLost) {
  Analysis a;
  memset(&a, 0, sizeof a);
  char buf[128];
  a.eval = EVAL_START;
  a.best_x = 0;
  a.best_y = 0;
  overlay_eval_string(&a, buf, sizeof buf);
  EXPECT_NE(strstr(buf, "START"), nullptr);
  a.eval = EVAL_SOLVED;
  overlay_eval_string(&a, buf, sizeof buf);
  EXPECT_STREQ(buf, "SOLVED");
  a.eval = EVAL_LOST;
  overlay_eval_string(&a, buf, sizeof buf);
  EXPECT_STREQ(buf, "LOST");
}
