/* reasoning_test.cc — reasoning_build: the pure Analysis -> ReasoningView
 * extraction that feeds the companion panel. */
#include "solver/reasoning.h"

#include <gtest/gtest.h>
#include <string.h>

#include "minesweeper/game.h"
#include "solver/engine.h"

namespace {

void mkboard(struct Board* b, int w, int h) {
  memset(b, 0, sizeof *b);
  b->width = w;
  b->height = h;
  b->mines = 10;
  b->status = GAME_PLAYING;
}

void mkanalysis(struct Analysis* a, const struct Board* b, double p) {
  memset(a, 0, sizeof *a);
  for (int i = 0; i < b->width * b->height; ++i) {
    a->cells[i].mine_prob = p;
  }
  a->eval = EVAL_GUESS;
  a->exact = true;
  a->best_x = 0;
  a->best_y = 0;
  a->best_prob = p;
}

void setp(struct Analysis* a, const struct Board* b, int x, int y, double p) {
  a->cells[game_index(b, x, y)].mine_prob = p;
}
void setgain(struct Analysis* a, const struct Board* b, int x, int y, int g) {
  a->cells[game_index(b, x, y)].info_gain = g;
}

}  // namespace

TEST(Reasoning, VerdictAndExactPassThrough) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  a.eval = EVAL_GUESS;
  a.exact = false;
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  EXPECT_EQ(v.verdict, EVAL_GUESS);
  EXPECT_FALSE(v.exact);
  EXPECT_EQ(v.mines_total, 10);
}

TEST(Reasoning, CountsProvenAndFrontier) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  a.cells[game_index(&b, 1, 1)].forced_safe = true;
  a.cells[game_index(&b, 2, 1)].forced_mine = true;
  a.cells[game_index(&b, 3, 1)].is_frontier = true;
  a.cells[game_index(&b, 1, 1)].is_frontier = true;
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  EXPECT_EQ(v.proven_safe, 1);
  EXPECT_EQ(v.proven_mine, 1);
  EXPECT_EQ(v.frontier, 2);
}

TEST(Reasoning, TookRiskierWhenBandPicksHigherGain) {
  /* Safest cell at 10% zero-gain; a 11% cell (within 2% band) with gain 4 wins.
   * reasoning mirrors solver_recommend_move, so the pick is the 11% cell. */
  struct Board b;
  mkboard(&b, 7, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);
  setp(&a, &b, 1, 1, 0.10);
  setgain(&a, &b, 1, 1, 0);
  setp(&a, &b, 5, 1, 0.11);
  setgain(&a, &b, 5, 1, 4);
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  ASSERT_TRUE(v.has_move);
  EXPECT_EQ(v.move_x, 5);
  EXPECT_EQ(v.move_y, 1);
  EXPECT_EQ(v.risk_pct, 11);
  EXPECT_EQ(v.safest_pct, 10);
  EXPECT_TRUE(v.took_riskier);
  EXPECT_EQ(v.pick_gain, 4);
}

TEST(Reasoning, NotRiskierWhenPickIsSafest) {
  struct Board b;
  mkboard(&b, 7, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);
  setp(&a, &b, 2, 1, 0.10);  // unique minimum, zero gain
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  ASSERT_TRUE(v.has_move);
  EXPECT_EQ(v.move_x, 2);
  EXPECT_FALSE(v.took_riskier);
  EXPECT_EQ(v.risk_pct, 10);
  EXPECT_EQ(v.safest_pct, 10);
}

TEST(Reasoning, StartMoveIsZeroRisk) {
  /* At EVAL_START the engine's mine_prob is the uniform density, but the first
   * click is guaranteed safe — the readout must show 0% risk, not the density. */
  struct Board b;
  mkboard(&b, 9, 9);
  struct Analysis a;
  mkanalysis(&a, &b, 0.123);  // uniform 10/81 density on every cell
  a.eval = EVAL_START;
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  ASSERT_TRUE(v.has_move);
  EXPECT_EQ(v.move_x, 0);
  EXPECT_EQ(v.move_y, 0);
  EXPECT_EQ(v.risk_pct, 0);
}

TEST(Reasoning, HoverCoveredCellFilled) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  setp(&a, &b, 4, 2, 0.25);
  a.cells[game_index(&b, 4, 2)].is_frontier = true;
  a.cells[game_index(&b, 4, 2)].info_gain = 2;
  struct ReasoningView v;
  reasoning_build(&b, &a, 4, 2, &v);
  ASSERT_TRUE(v.hover_valid);
  EXPECT_EQ(v.hover_x, 4);
  EXPECT_EQ(v.hover_pct, 25);
  EXPECT_TRUE(v.hover_frontier);
  EXPECT_EQ(v.hover_gain, 2);
}

TEST(Reasoning, HoverRevealedOrOutOfRangeInvalid) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  b.cells[game_index(&b, 1, 1)].revealed = true;
  struct ReasoningView v;
  reasoning_build(&b, &a, 1, 1, &v);  // revealed
  EXPECT_FALSE(v.hover_valid);
  reasoning_build(&b, &a, -1, -1, &v);  // none
  EXPECT_FALSE(v.hover_valid);
  reasoning_build(&b, &a, 99, 99, &v);  // out of range
  EXPECT_FALSE(v.hover_valid);
}
