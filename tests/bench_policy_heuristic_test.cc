/* bench_policy_heuristic_test.cc — progress-aware heuristic policy (Stream
 * B.2).
 *
 * Builds Board+Analysis fixtures directly (no engine run) to isolate the
 * tie-break: cascade dominance, open-far preference, never-forced-mine, band
 * respects the minimum risk, determinism.
 */
#include <gtest/gtest.h>
#include <string.h>

#include "minesweeper/game.h"
#include "policy_heuristic.h"
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
  a->best_x = 0;
  a->best_y = 0;
  a->best_prob = p;
}

void setp(struct Analysis* a, const struct Board* b, int x, int y, double p) {
  a->cells[game_index(b, x, y)].mine_prob = p;
}

void reveal(struct Board* b, int x, int y) {
  b->cells[game_index(b, x, y)].revealed = true;
}

int cheb_to_revealed(const struct Board* b, int x, int y) {
  int best = 999;
  for (int ry = 0; ry < b->height; ++ry) {
    for (int rx = 0; rx < b->width; ++rx) {
      if (!b->cells[game_index(b, rx, ry)].revealed) continue;
      int ax = rx > x ? rx - x : x - rx;
      int ay = ry > y ? ry - y : y - ry;
      int d = ax > ay ? ax : ay;
      if (d < best) best = d;
    }
  }
  return best;
}

}  // namespace

/* Among equal-risk cells, a cell whose covered neighbors are LESS mine-likely
 * (higher P(reveal 0)) wins — it dominates on cascade and ties on the rest. */
TEST(PolicyHeuristic, CascadeDominanceWins) {
  struct Board b;
  mkboard(&b, 7, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5); /* non-candidate default */

  /* C1=(1,1): neighbors very mine-likely (low cascade). C2=(5,1): neighbors
   * unlikely (high cascade). Both own risk 0.3 = the minimum. Non-overlapping
   * neighborhoods; both interior with 8 covered neighbors; no reveals → equal
   * open-far. So C2 dominates on cascade only → must be chosen. */
  setp(&a, &b, 1, 1, 0.3);
  setp(&a, &b, 5, 1, 0.3);
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      setp(&a, &b, 1 + dx, 1 + dy, 0.8); /* C1 neighbors: low cascade */
      setp(&a, &b, 5 + dx, 1 + dy, 0.4); /* C2 neighbors: high cascade */
    }
  }

  struct Move mv;
  ASSERT_EQ(policy_heuristic_select(&b, &a, &mv), 0);
  EXPECT_EQ(mv.x, 5);
  EXPECT_EQ(mv.y, 1);
}

/* With uniform risk everywhere, the policy reveals into open space (far from
 * the solved cluster) rather than hugging the frontier. */
TEST(PolicyHeuristic, OpenFarPreferred) {
  struct Board b;
  mkboard(&b, 5, 5);
  reveal(&b, 0, 0);
  reveal(&b, 1, 0);
  reveal(&b, 0, 1);
  struct Analysis a;
  mkanalysis(&a, &b, 0.2); /* all covered cells equal risk */
  a.best_x = 2;
  a.best_y = 0;

  struct Move mv;
  ASSERT_EQ(policy_heuristic_select(&b, &a, &mv), 0);
  EXPECT_GE(cheb_to_revealed(&b, mv.x, mv.y), 3); /* deep into blank space */
  EXPECT_FALSE(b.cells[game_index(&b, mv.x, mv.y)].revealed);
}

/* Never selects a proven mine; chosen risk equals the minimum (band=0). */
TEST(PolicyHeuristic, NeverForcedMineAndRespectsMinRisk) {
  struct Board b;
  mkboard(&b, 4, 4);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  int mineIdx = game_index(&b, 2, 2);
  a.cells[mineIdx].mine_prob = 1.0;
  a.cells[mineIdx].forced_mine = true;

  struct Move mv;
  ASSERT_EQ(policy_heuristic_select(&b, &a, &mv), 0);
  int chosen = game_index(&b, mv.x, mv.y);
  EXPECT_NE(chosen, mineIdx);
  EXPECT_FALSE(a.cells[chosen].forced_mine);
  EXPECT_DOUBLE_EQ(a.cells[chosen].mine_prob, 0.3); /* == pmin, band 0 */
}

TEST(PolicyHeuristic, Deterministic) {
  struct Board b;
  mkboard(&b, 6, 4);
  reveal(&b, 0, 0);
  struct Analysis a;
  mkanalysis(&a, &b, 0.25);
  a.best_x = 1;
  a.best_y = 0;

  struct Move m1;
  struct Move m2;
  ASSERT_EQ(policy_heuristic_select(&b, &a, &m1), 0);
  ASSERT_EQ(policy_heuristic_select(&b, &a, &m2), 0);
  EXPECT_EQ(m1.x, m2.x);
  EXPECT_EQ(m1.y, m2.y);
}

/* Opening (EVAL_START) is pinned to the engine's pick (CRN with baseline). */
TEST(PolicyHeuristic, OpeningPinnedToEnginePick) {
  struct Board b;
  mkboard(&b, 9, 9);
  struct Analysis a;
  mkanalysis(&a, &b, 0.123);
  a.eval = EVAL_START;
  a.best_x = 0;
  a.best_y = 0;

  struct Move mv;
  ASSERT_EQ(policy_heuristic_select(&b, &a, &mv), 0);
  EXPECT_EQ(mv.x, 0);
  EXPECT_EQ(mv.y, 0);
}
