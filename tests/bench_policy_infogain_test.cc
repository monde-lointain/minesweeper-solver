/* bench_policy_infogain_test.cc — info-gain guess policy (paper's Inf(x)).
 *
 * Builds Board+Analysis fixtures directly (info_gain set by hand) to isolate
 * the tie-break: info_gain dominates cascade, min-risk band is respected,
 * proven mines are never chosen, cascade breaks info_gain ties, determinism,
 * opening pinned to the engine pick.
 */
#include <gtest/gtest.h>
#include <string.h>

#include "minesweeper/game.h"
#include "policy.h"
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

void set_neighbors(struct Analysis* a, const struct Board* b, int cx, int cy,
                   double p) {
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      int x = cx + dx;
      int y = cy + dy;
      if (x < 0 || y < 0 || x >= b->width || y >= b->height) continue;
      setp(a, b, x, y, p);
    }
  }
}

}  // namespace

/* Among equal-risk cells in the band, the higher info_gain wins even when the
 * other cell has a stronger cascade (info_gain outranks cascade). */
TEST(PolicyInfogain, InfoGainOutranksCascade) {
  struct Board b;
  mkboard(&b, 7, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);

  /* Candidates are the unique minima (0.1); neighbors stay above the band so
   * they are not competitors, but their probs set the cascade strength. */
  setp(&a, &b, 1, 1, 0.1);          /* C1: strong cascade, weak info_gain */
  setp(&a, &b, 5, 1, 0.1);          /* C2: weak cascade, strong info_gain */
  set_neighbors(&a, &b, 1, 1, 0.5); /* C1 neighbors -> higher cascade */
  set_neighbors(&a, &b, 5, 1, 0.9); /* C2 neighbors -> lower cascade */
  setgain(&a, &b, 1, 1, 1);
  setgain(&a, &b, 5, 1, 4);

  struct Pt mv;
  ASSERT_EQ(policy_select(POLICY_INFOGAIN, &b, &a, &mv), 0);
  EXPECT_EQ(mv.x, 5);
  EXPECT_EQ(mv.y, 1);
}

/* Min risk is the primary key: a low-risk cell with zero info_gain beats a
 * higher-risk cell (outside the band) with large info_gain. */
TEST(PolicyInfogain, MinRiskRespectedOverInfoGain) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);

  setp(&a, &b, 1, 1, 0.2); /* the minimum risk, no forcing */
  setgain(&a, &b, 1, 1, 0);
  setp(&a, &b, 3, 1, 0.5); /* higher risk but huge info_gain */
  setgain(&a, &b, 3, 1, 9);

  struct Pt mv;
  ASSERT_EQ(policy_select(POLICY_INFOGAIN, &b, &a, &mv), 0);
  EXPECT_EQ(mv.x, 1);
  EXPECT_EQ(mv.y, 1);
  EXPECT_DOUBLE_EQ(a.cells[game_index(&b, mv.x, mv.y)].mine_prob, 0.2);
}

/* Cascade breaks ties only when info_gain is equal. */
TEST(PolicyInfogain, CascadeBreaksInfoGainTie) {
  struct Board b;
  mkboard(&b, 7, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);

  setp(&a, &b, 1, 1, 0.1);
  setp(&a, &b, 5, 1, 0.1);
  setgain(&a, &b, 1, 1, 2);
  setgain(&a, &b, 5, 1, 2);         /* equal info_gain */
  set_neighbors(&a, &b, 1, 1, 0.9); /* low cascade */
  set_neighbors(&a, &b, 5, 1, 0.5); /* higher cascade -> wins */

  struct Pt mv;
  ASSERT_EQ(policy_select(POLICY_INFOGAIN, &b, &a, &mv), 0);
  EXPECT_EQ(mv.x, 5);
  EXPECT_EQ(mv.y, 1);
}

/* A proven mine is never chosen, even with a large info_gain. */
TEST(PolicyInfogain, NeverForcedMine) {
  struct Board b;
  mkboard(&b, 4, 4);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  int mi = game_index(&b, 2, 2);
  a.cells[mi].mine_prob = 1.0;
  a.cells[mi].forced_mine = true;
  a.cells[mi].info_gain = 99;

  struct Pt mv;
  ASSERT_EQ(policy_select(POLICY_INFOGAIN, &b, &a, &mv), 0);
  int chosen = game_index(&b, mv.x, mv.y);
  EXPECT_NE(chosen, mi);
  EXPECT_FALSE(a.cells[chosen].forced_mine);
}

TEST(PolicyInfogain, Deterministic) {
  struct Board b;
  mkboard(&b, 6, 4);
  struct Analysis a;
  mkanalysis(&a, &b, 0.25);
  setgain(&a, &b, 2, 1, 3);
  setgain(&a, &b, 4, 2, 3);

  struct Pt m1;
  struct Pt m2;
  ASSERT_EQ(policy_select(POLICY_INFOGAIN, &b, &a, &m1), 0);
  ASSERT_EQ(policy_select(POLICY_INFOGAIN, &b, &a, &m2), 0);
  EXPECT_EQ(m1.x, m2.x);
  EXPECT_EQ(m1.y, m2.y);
}

/* Opening (EVAL_START) is pinned to the engine's pick (CRN with baseline). */
TEST(PolicyInfogain, OpeningPinnedToEnginePick) {
  struct Board b;
  mkboard(&b, 9, 9);
  struct Analysis a;
  mkanalysis(&a, &b, 0.123);
  a.eval = EVAL_START;
  a.best.x = 0;
  a.best.y = 0;

  struct Pt mv;
  ASSERT_EQ(policy_select(POLICY_INFOGAIN, &b, &a, &mv), 0);
  EXPECT_EQ(mv.x, 0);
  EXPECT_EQ(mv.y, 0);
}
