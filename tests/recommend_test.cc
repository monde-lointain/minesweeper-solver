/* recommend_test.cc — solver_recommend_move: the shared min-risk + info-gain
 * move pick. The bench policy and the GUI overlay both call it, so this is the
 * contract both depend on (tie-break, min-risk primacy, opening pin, terminal).
 */
#include "solver/recommend.h"

#include <gtest/gtest.h>
#include <string.h>

#include "minesweeper/game.h"
#include "solver/engine.h"

namespace {

void mkboard(struct Board* b, int w, int h) {
  memset(b, 0, sizeof *b);
  b->width = w;
  b->height = h;
  b->status = GAME_PLAYING;
}

void mkanalysis(struct Analysis* a, const struct Board* b, double p) {
  memset(a, 0, sizeof *a);
  for (int i = 0; i < b->width * b->height; ++i) {
    a->cells[i].mine_prob = p;
  }
  a->eval = EVAL_GUESS;
  a->best.x = 0;
  a->best.y = 0;
  a->best_prob = p;
}

void setp(struct Analysis* a, const struct Board* b, int x, int y, double p) {
  a->cells[game_index(b, x, y)].mine_prob = p;
}
void setgain(struct Analysis* a, const struct Board* b, int x, int y, int g) {
  a->cells[game_index(b, x, y)].info_gain = g;
}

/* Adapter preserving the int-out call shape these tests were written against.
 */
int reco(const struct Board* b, const struct Analysis* a, int* x, int* y) {
  struct Pt p = {-1, -1};
  int rc = solver_recommend_move(b, a, &p);
  *x = p.x;
  *y = p.y;
  return rc;
}

}  // namespace

/* Among equal-risk cells in the band, the higher info_gain wins. */
TEST(Recommend, InfoGainBreaksTie) {
  struct Board b;
  mkboard(&b, 7, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);
  setp(&a, &b, 1, 1, 0.1);
  setp(&a, &b, 5, 1, 0.1);
  setgain(&a, &b, 1, 1, 1);
  setgain(&a, &b, 5, 1, 4);
  int x = -1;
  int y = -1;
  ASSERT_EQ(reco(&b, &a, &x, &y), 0);
  EXPECT_EQ(x, 5);
  EXPECT_EQ(y, 1);
}

/* Min risk is primary: a low-risk zero-gain cell beats a higher-risk
 * (outside-band) huge-gain cell. */
TEST(Recommend, MinRiskPrimary) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);
  setp(&a, &b, 1, 1, 0.2);
  setgain(&a, &b, 1, 1, 0);
  setp(&a, &b, 3, 1, 0.5);
  setgain(&a, &b, 3, 1, 9);
  int x = -1;
  int y = -1;
  ASSERT_EQ(reco(&b, &a, &x, &y), 0);
  EXPECT_EQ(x, 1);
  EXPECT_EQ(y, 1);
}

/* A proven mine is never chosen, even with a large info_gain. */
TEST(Recommend, NeverForcedMine) {
  struct Board b;
  mkboard(&b, 4, 4);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  int mi = game_index(&b, 2, 2);
  a.cells[mi].mine_prob = 1.0;
  a.cells[mi].forced_mine = true;
  a.cells[mi].info_gain = 99;
  int x = -1;
  int y = -1;
  ASSERT_EQ(reco(&b, &a, &x, &y), 0);
  EXPECT_NE(game_index(&b, x, y), mi);
}

/* Opening (EVAL_START) is pinned to the engine pick (CRN parity with baseline).
 */
TEST(Recommend, OpeningPinned) {
  struct Board b;
  mkboard(&b, 9, 9);
  struct Analysis a;
  mkanalysis(&a, &b, 0.123);
  a.eval = EVAL_START;
  a.best.x = 0;
  a.best.y = 0;
  int x = -1;
  int y = -1;
  ASSERT_EQ(reco(&b, &a, &x, &y), 0);
  EXPECT_EQ(x, 0);
  EXPECT_EQ(y, 0);
}

/* No covered cell (terminal) -> -1. */
TEST(Recommend, NoneWhenTerminal) {
  struct Board b;
  mkboard(&b, 4, 4);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  a.best.x = -1;
  a.best.y = -1;
  int x = -1;
  int y = -1;
  EXPECT_EQ(reco(&b, &a, &x, &y), -1);
}

TEST(Recommend, Deterministic) {
  struct Board b;
  mkboard(&b, 6, 4);
  struct Analysis a;
  mkanalysis(&a, &b, 0.25);
  setgain(&a, &b, 2, 1, 3);
  setgain(&a, &b, 4, 2, 3);
  int x1 = -1;
  int y1 = -1;
  int x2 = -1;
  int y2 = -1;
  ASSERT_EQ(reco(&b, &a, &x1, &y1), 0);
  ASSERT_EQ(reco(&b, &a, &x2, &y2), 0);
  EXPECT_EQ(x1, x2);
  EXPECT_EQ(y1, y2);
}

/* At EVAL_SAFE the risk band is suppressed: a proven-safe (0%) cell is chosen
 * even when an in-band (0,band] cell has strictly higher progress. Without the
 * gate the risky corner (5,0) — three proven-safe neighbors => cascade 1 =>
 * highest progress — would win. A paired 200k-Expert test favored the gate
 * (McNemar chi2=17.9, p<0.001). */
TEST(Recommend, SafeGateRejectsRiskyBand) {
  struct Board b;
  mkboard(&b, 6, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5); /* most cells high-risk -> outside the band */
  a.eval = EVAL_SAFE;
  setp(&a, &b, 5, 0, 0.01); /* risky, in band, max progress */
  setp(&a, &b, 4, 0, 0.0);
  setp(&a, &b, 5, 1, 0.0);
  setp(&a, &b, 4, 1, 0.0);
  setp(&a, &b, 0, 2, 0.0); /* a far proven-safe cell, lower progress */
  int x = -1;
  int y = -1;
  ASSERT_EQ(reco(&b, &a, &x, &y), 0);
  EXPECT_LT(a.cells[game_index(&b, x, y)].mine_prob, 1e-9);
  EXPECT_FALSE(x == 5 && y == 0);
}
