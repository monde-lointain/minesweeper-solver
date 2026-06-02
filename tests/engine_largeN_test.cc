/* engine_largeN_test.cc — correctness oracle for the normalized binomial path
 * in the OVERFLOW regime, which neither the brute-force oracle (small boards,
 * no overflow) nor the golden corpus (expert-size, small interior) reaches.
 *
 * Two independent checks on 100x100/2500 boards with a tiny exact frontier and
 * a huge interior (interior_n ~ 1e4, so raw C(interior_n, ~2500) ~ 1e2000+
 * would overflow a 64-bit double — the case the old binom_ld needed long double
 * for):
 *
 *  1. Conservation: for a FULLY exact analysis, the per-cell mine marginals sum
 *     to the total mine count (every valid configuration has exactly `mines`
 *     mines, all on covered cells). A size-independent analytic invariant that
 *     no internal detail can fake.
 *
 *  2. Long-double agreement: the SAME engine sources recompiled with
 *     ereal=long double (range ~1e4932, never overflows here) must agree with
 *     the production double engine to ~1e-9 — isolating normalization/clamping
 *     error from the (proven-irrelevant) precision difference.
 *
 * The long-double engine is solver_lib_ld: identical sources, public symbols
 * renamed `_ld` (see tests/CMakeLists.txt) so both link into one executable.
 */
#include <gtest/gtest.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "minesweeper/game.h"
#include "solver/engine.h"

/* long-double reference engine (renamed symbols from solver_lib_ld). */
struct SolverScratch* solver_scratch_create_ld(void);
void solver_scratch_destroy_ld(struct SolverScratch* s);
void solver_analyze_ld(const struct Board* b, struct Analysis* out,
                       struct SolverScratch* s);

namespace {

enum { W = 100, H = 100, MINES = 2500, NCELL = W * H };

uint32_t nextr(uint32_t* s) {
  *s = *s * 1103515245u + 12345u;
  return *s;
}

int true_adjacent(const unsigned char* mine, int x, int y) {
  int n = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      int nx = x + dx;
      int ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
        continue;
      }
      if (mine[ny * W + nx]) {
        ++n;
      }
    }
  }
  return n;
}

/* 100x100 board, MINES mines (seeded). Reveals `want` non-mine cells with
 * adjacent>0, scanning row-major from (rx,ry), so the frontier stays small
 * (exact) while the interior stays huge. */
void build(uint32_t seed, int rx, int ry, int want, Board* b) {
  memset(b, 0, sizeof *b);
  b->width = W;
  b->height = H;
  b->mines = MINES;
  b->status = GAME_PLAYING;
  b->mines_placed = true;

  static unsigned char mine[NCELL];
  memset(mine, 0, sizeof mine);
  uint32_t s = seed;
  int placed = 0;
  while (placed < MINES) {
    int p = (int)((nextr(&s) >> 8) % (unsigned)NCELL);
    if (!mine[p]) {
      mine[p] = 1;
      ++placed;
    }
  }
  for (int i = 0; i < NCELL; ++i) {
    b->cells[i].mine = mine[i] != 0;
  }
  int revealed = 0;
  int start = ry * W + rx;
  for (int i = start; i < NCELL && revealed < want; ++i) {
    if (mine[i]) {
      continue;
    }
    int adj = true_adjacent(mine, i % W, i / W);
    if (adj == 0) {
      continue; /* need a constraint */
    }
    b->cells[i].revealed = true;
    b->cells[i].adjacent = (uint8_t)adj;
    ++revealed;
  }
  b->revealed_count = revealed;
}

double sum_covered_prob(const Board* b, const Analysis* a) {
  double s = 0.0;
  for (int i = 0; i < NCELL; ++i) {
    if (!b->cells[i].revealed) {
      s += a->cells[i].mine_prob;
    }
  }
  return s;
}

int interior_seen(const Board* b, const Analysis* a) {
  int n = 0;
  for (int i = 0; i < NCELL; ++i) {
    if (!b->cells[i].revealed && !a->cells[i].is_frontier) {
      ++n;
    }
  }
  return n;
}

struct Cfg {
  uint32_t seed;
  int rx;
  int ry;
  int want;
};

}  // namespace

TEST(EngineLargeN, ConservationAndLongDoubleAgreement) {
  const Cfg cfgs[] = {
      {0x000A11CEu, 0, 0, 1},
      {0x0000BEEFu, 10, 10, 3},
      {0x00C0FFEEu, 50, 50, 4},
      {0x0D15EA5Eu, 80, 5, 2},
  };
  const int ncfg = (int)(sizeof cfgs / sizeof cfgs[0]);

  Board* b = (Board*)malloc(sizeof *b);
  Analysis* ad = (Analysis*)malloc(sizeof *ad);
  Analysis* al = (Analysis*)malloc(sizeof *al);
  SolverScratch* sd = solver_scratch_create();
  SolverScratch* sl = solver_scratch_create_ld();
  ASSERT_NE(b, nullptr);
  ASSERT_NE(ad, nullptr);
  ASSERT_NE(al, nullptr);
  ASSERT_NE(sd, nullptr);
  ASSERT_NE(sl, nullptr);

  for (int c = 0; c < ncfg; ++c) {
    SCOPED_TRACE(c);
    build(cfgs[c].seed, cfgs[c].rx, cfgs[c].ry, cfgs[c].want, b);

    solver_analyze(b, ad, sd);
    solver_analyze_ld(b, al, sl);

    /* must exercise the exact large-interior path (else the test is vacuous) */
    EXPECT_TRUE(ad->exact);
    EXPECT_TRUE(al->exact);
    EXPECT_GT(interior_seen(b, ad), 5000);

    /* every probability finite and in [0,1] (no overflow -> nan/inf) */
    for (int i = 0; i < NCELL; ++i) {
      if (b->cells[i].revealed) {
        continue;
      }
      double p = ad->cells[i].mine_prob;
      ASSERT_TRUE(isfinite(p));
      ASSERT_GE(p, 0.0);
      ASSERT_LE(p, 1.0);
    }

    /* (1) conservation: sum of covered marginals == total mines (exact path) */
    double sd_sum = sum_covered_prob(b, ad);
    EXPECT_NEAR(sd_sum, (double)MINES, (double)MINES * 1e-6);

    /* (2) long-double-with-table vs double-with-table agree to ~1e-9 */
    double max_abs = 0.0;
    for (int i = 0; i < NCELL; ++i) {
      if (b->cells[i].revealed) {
        continue;
      }
      double d = fabs(ad->cells[i].mine_prob - al->cells[i].mine_prob);
      if (d > max_abs) {
        max_abs = d;
      }
    }
    EXPECT_LE(max_abs, 1e-9);
  }

  solver_scratch_destroy(sd);
  solver_scratch_destroy_ld(sl);
  free(b);
  free(ad);
  free(al);
}
