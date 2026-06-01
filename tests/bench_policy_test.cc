/* bench_policy_test.cc — Stream B.1.
 * POLICY_BASELINE forwards the engine's best move and never picks a revealed
 * cell; returns -1 when there is no best move.
 */
#include <gtest/gtest.h>
#include <stdlib.h>

#include "minesweeper/game.h"
#include "policy.h"
#include "solver/engine.h"

TEST(BenchPolicy, BaselineMatchesEngineBestAndIsCovered) {
  struct Board b;
  struct Rng rng;
  rng.fn = NULL;
  rng.ctx = NULL;
  rng.seed = 12345u;
  game_reset(&b, 9, 9, 10, &rng);
  game_reveal(&b, 0, 0); /* places mines avoiding (0,0); board now PLAYING */

  struct SolverScratch* sc = solver_scratch_create();
  ASSERT_NE(sc, nullptr);
  struct Analysis* a = (struct Analysis*)malloc(sizeof *a);
  ASSERT_NE(a, nullptr);
  solver_analyze(&b, a, sc);

  struct Move mv;
  int rc = policy_select(POLICY_BASELINE, &b, a, &mv);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(mv.x, a->best.x);
  EXPECT_EQ(mv.y, a->best.y);
  EXPECT_FALSE(b.cells[game_index(&b, mv.x, mv.y)].revealed);

  free(a);
  solver_scratch_destroy(sc);
}

TEST(BenchPolicy, ReturnsNegativeWhenNoBestMove) {
  struct Board b;
  struct Rng rng;
  rng.fn = NULL;
  rng.ctx = NULL;
  rng.seed = 1u;
  game_reset(&b, 9, 9, 10, &rng);

  struct Analysis* a = (struct Analysis*)calloc(1, sizeof *a);
  ASSERT_NE(a, nullptr);
  a->best.x = -1;
  a->best.y = -1;

  struct Move mv;
  EXPECT_EQ(policy_select(POLICY_BASELINE, &b, a, &mv), -1);

  free(a);
}
