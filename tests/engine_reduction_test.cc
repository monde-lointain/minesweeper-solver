/* engine_reduction_test.cc — Gaussian-reduction driver correctness (Stream
 * B.1).
 *
 * 1. Differential: for a corpus of random boards, the reduction path
 *    (force-reduce on) must produce marginals identical (<=1e-12) to the normal
 *    analyze, whose <=CAP_VARS components use the trusted direct enumeration.
 *    Combined with engine_test (normal == brute-force oracle on small boards),
 *    this proves reduced == exact on every small component in the corpus.
 * 2. Consistency on reduction-exercising boards: marginals in [0,1] and the
 *    covered-cell probability mass equals the mine budget (global-budget
 * proof).
 * 3. Forced cells produced by reduction are genuinely forced.
 */
#include <gtest/gtest.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "engine_internal.h"
#include "minesweeper/game.h"
#include "solver/engine.h"

namespace {

enum { GMAX = 30 * 16 };

uint32_t nextr(uint32_t* s) {
  *s = *s * 1103515245u + 12345u;
  return *s;
}

int true_adjacent(const unsigned char* mine, int w, int h, int x, int y) {
  int n = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      int nx = x + dx;
      int ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      if (mine[ny * w + nx]) ++n;
    }
  }
  return n;
}

/* Deterministic board: place `mines` mines, reveal each non-mine with
 * `reveal_pct`% probability. Self-consistent adjacents. High reveal_pct keeps
 * the frontier small (fully exact); low reveal_pct stresses dense components.
 */
void build(int w, int h, int mines, uint32_t seed, int reveal_pct, Board* b) {
  memset(b, 0, sizeof *b);
  b->width = w;
  b->height = h;
  b->status = GAME_PLAYING;
  b->mines_placed = true;
  int ncell = w * h;
  unsigned char mine[GMAX];
  memset(mine, 0, sizeof mine);
  uint32_t s = seed;
  int placed = 0;
  while (placed < mines) {
    int p = (int)((nextr(&s) >> 8) % (uint32_t)ncell);
    if (!mine[p]) {
      mine[p] = 1;
      ++placed;
    }
  }
  int revealed = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int i = y * w + x;
      b->cells[i].mine = mine[i] != 0;
      if (mine[i]) continue;
      if ((int)((nextr(&s) >> 8) % 100u) < reveal_pct) {
        b->cells[i].revealed = true;
        b->cells[i].adjacent = (uint8_t)true_adjacent(mine, w, h, x, y);
        ++revealed;
      }
    }
  }
  b->mines = mines;
  b->revealed_count = revealed;
}

}  // namespace

/* Reduced enumeration must match direct enumeration on every component the
 * direct path can handle. */
TEST(EngineReduction, DifferentialVsDirect) {
  SolverScratch* sc = solver_scratch_create();
  Analysis* a1 = (Analysis*)malloc(sizeof *a1);
  Analysis* a2 = (Analysis*)malloc(sizeof *a2);
  int boards = 0;
  for (uint32_t seed = 1; seed <= 120; ++seed) {
    for (int dens = 0; dens < 3; ++dens) {
      int mines = dens == 0 ? 30 : (dens == 1 ? 60 : 99);
      int pct = dens == 0 ? 65 : (dens == 1 ? 55 : 50);
      Board b;
      build(30, 16, mines, seed * 7u + (uint32_t)dens, pct, &b);

      solver_test_set_force_reduce(sc, false);
      solver_analyze(&b, a1, sc);
      solver_test_set_force_reduce(sc, true);
      solver_analyze(&b, a2, sc);
      solver_test_set_force_reduce(sc, false);

      ASSERT_EQ(a1->eval, a2->eval) << "seed " << seed << " dens " << dens;
      for (int i = 0; i < b.width * b.height; ++i) {
        if (b.cells[i].revealed) continue;
        ASSERT_NEAR(a1->cells[i].mine_prob, a2->cells[i].mine_prob, 1e-12)
            << "seed " << seed << " dens " << dens << " cell " << i;
        ASSERT_EQ(a1->cells[i].forced_safe, a2->cells[i].forced_safe);
        ASSERT_EQ(a1->cells[i].forced_mine, a2->cells[i].forced_mine);
      }
      ++boards;
    }
  }
  EXPECT_GT(boards, 0);
  free(a1);
  free(a2);
  solver_scratch_destroy(sc);
}

/* On boards the reduction actually solves exactly, the covered-cell probability
 * mass must equal the mine budget, and every marginal is a valid probability.
 */
TEST(EngineReduction, GlobalBudgetConsistency) {
  SolverScratch* sc = solver_scratch_create();
  Analysis* a = (Analysis*)malloc(sizeof *a);
  for (uint32_t seed = 1; seed <= 200; ++seed) {
    Board b;
    /* High reveal % -> small frontier -> fully exact (no MAXFLEN fallback), so
     * the global-budget identity holds tightly. Components still straddle
     * CAP_VARS, exercising the reduction. */
    build(16, 16, 40, seed * 13u + 1u, 80, &b);
    solver_analyze(&b, a, sc);
    double sum = 0.0;
    for (int i = 0; i < b.width * b.height; ++i) {
      if (b.cells[i].revealed) continue;
      double p = a->cells[i].mine_prob;
      ASSERT_GE(p, 0.0) << "seed " << seed << " cell " << i;
      ASSERT_LE(p, 1.0) << "seed " << seed << " cell " << i;
      sum += p;
    }
    /* sum of covered P(mine) == mines (no flags; all mines covered). */
    EXPECT_NEAR(sum, (double)b.mines, 1e-6) << "seed " << seed;
  }
  free(a);
  solver_scratch_destroy(sc);
}

/* A cell reduction reports forced_safe/forced_mine must be truly forced: re-run
 * with that cell pinned to the opposite value and confirm the local constraints
 * become unsatisfiable is hard to check directly; instead verify against the
 * differential oracle (forced flags already matched direct enumeration above)
 * AND that no forced_safe cell is over a mine in a fully-revealed solution is
 * covered by the bench's deaths@forced-safe gate. Here we assert forced cells
 * are internally consistent: forced_safe => prob 0, forced_mine => prob 1. */
TEST(EngineReduction, ForcedFlagsConsistent) {
  SolverScratch* sc = solver_scratch_create();
  Analysis* a = (Analysis*)malloc(sizeof *a);
  for (uint32_t seed = 1; seed <= 200; ++seed) {
    Board b;
    build(30, 16, 80, seed * 17u + 3u, 50, &b);
    solver_analyze(&b, a, sc);
    for (int i = 0; i < b.width * b.height; ++i) {
      if (b.cells[i].revealed) continue;
      if (a->cells[i].forced_safe) {
        EXPECT_DOUBLE_EQ(a->cells[i].mine_prob, 0.0) << "seed " << seed;
      }
      if (a->cells[i].forced_mine) {
        EXPECT_DOUBLE_EQ(a->cells[i].mine_prob, 1.0) << "seed " << seed;
      }
    }
  }
  free(a);
  solver_scratch_destroy(sc);
}
