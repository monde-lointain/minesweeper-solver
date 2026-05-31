/* golden_test.cc — characterization gate for the engine refactor.
 *
 * The brute-force oracle in engine_test.cc only exercises SMALL exact boards
 * (<=16 covered cells). It never reaches the fallback / over-budget /
 * large-component paths. This test freezes the CURRENT solver_analyze output on
 * a deterministic corpus of large/dense boards (incl. expert 30x16 with
 * >24-var components that trip the fallback) so the refactor can be proven
 * behaviour-preserving on exactly those uncovered paths.
 *
 * It does NOT assert correctness — only "same as before". Each board is reduced
 * to scalars sensitive to every covered cell (position- and square-weighted
 * probability sums + forced counts) plus the headline verdict fields.
 *
 * Regenerate the baked baseline after an INTENTIONAL behaviour change:
 *   GOLDEN_CAPTURE=1 ./build/tests/golden_test
 * then paste the printed block over kExpected[] below.
 */
#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minesweeper/game.h"
#include "solver/engine.h"

namespace {

enum { GMAX = 30 * 16 }; /* largest corpus board */

struct Spec {
  const char* name;
  int w;
  int h;
  int mines;
  unsigned int seed;
  unsigned int
      reveal_bit; /* mask vs prng to decide reveal of a non-mine cell */
};

/* Corpus: large/dense boards that the small-exact oracle cannot cover. The
 * first four are expert-sized (480 cells) so row/region frontiers exceed
 * CAP_VARS=24 and force the fallback path; the last is a small overlap sanity.
 */
const Spec kCorpus[] = {
    {"expert_dense_a", 30, 16, 99, 0x000A11CEu, 1u},
    {"expert_mid_b", 30, 16, 60, 0x00000B0Bu, 2u},
    {"expert_sparse_c", 30, 16, 40, 0x00C0FFEEu, 4u},
    {"tall_expert_d", 16, 30, 99, 0x0D15EA5Eu, 1u},
    {"small_e", 9, 9, 10, 0x00001234u, 2u},
};
const int kNCorpus = (int)(sizeof kCorpus / sizeof kCorpus[0]);

struct Stats {
  int eval;
  int best_x;
  int best_y;
  int interior_count;
  int n_forced_safe;
  int n_forced_mine;
  double best_prob;
  double interior_prob;
  double sum_p;  /* sum of mine_prob over covered cells */
  double sum_ip; /* sum of index*mine_prob (position-weighted) */
  double sum_p2; /* sum of mine_prob^2 */
};

unsigned int nextr(unsigned int* s) {
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

/* Build a deterministic board: place `mines` mines, reveal each non-mine cell
 * with ~50% probability keyed to the spec bit. Self-consistent adjacents. */
void build(const Spec& sp, Board* b) {
  memset(b, 0, sizeof *b);
  b->width = sp.w;
  b->height = sp.h;
  b->status = GAME_PLAYING;
  b->mines_placed = true;
  int ncell = sp.w * sp.h;
  unsigned char mine[GMAX];
  memset(mine, 0, sizeof mine);
  unsigned int s = sp.seed;
  int placed = 0;
  while (placed < sp.mines) {
    int p = (int)((nextr(&s) >> 8) % (unsigned)ncell);
    if (!mine[p]) {
      mine[p] = 1;
      ++placed;
    }
  }
  int revealed = 0;
  for (int y = 0; y < sp.h; ++y) {
    for (int x = 0; x < sp.w; ++x) {
      int i = y * sp.w + x;
      Cell* c = &b->cells[i];
      c->mine = mine[i] != 0;
      if (mine[i]) continue;
      if (nextr(&s) & sp.reveal_bit) {
        c->revealed = true;
        c->adjacent = (uint8_t)true_adjacent(mine, sp.w, sp.h, x, y);
        ++revealed;
      }
    }
  }
  b->mines = sp.mines;
  b->revealed_count = revealed;
}

void measure(const Board* b, const Analysis* a, Stats* st) {
  memset(st, 0, sizeof *st);
  st->eval = a->eval;
  st->best_x = a->best_x;
  st->best_y = a->best_y;
  st->interior_count = a->interior_count;
  st->best_prob = a->best_prob;
  st->interior_prob = a->interior_prob;
  int ncell = b->width * b->height;
  for (int i = 0; i < ncell; ++i) {
    if (b->cells[i].revealed) continue;
    double p = a->cells[i].mine_prob;
    st->sum_p += p;
    st->sum_ip += (double)i * p;
    st->sum_p2 += p * p;
    if (a->cells[i].forced_safe) ++st->n_forced_safe;
    if (a->cells[i].forced_mine) ++st->n_forced_mine;
  }
}

/* --- BAKED BASELINE (captured from current solver_analyze) -----------------
 */
const Stats kExpected[] = {
    {1, 27, 0, 10, 9, 13, 0, 1, 145.5673611111111, 35362.076388888891,
     79.282843846450618}, /* expert_dense_a */
    {2, 15, 0, 20, 0, 20, 0.20000000000000001, 1, 141.50805555555556,
     34087.257222222222, 79.797755478395061}, /* expert_mid_b */
    {1, 18, 12, 40, 7, 2, 0, 0.48360655737704916, 107.18626984126976,
     26138.610751366119, 50.20391470115856}, /* expert_sparse_c */
    {1, 0, 24, 3, 5, 6, 0, 1, 140.61388888888888, 34863.770833333328,
     72.038757716049389}, /* tall_expert_d */
    {1, 2, 0, 5, 3, 1, 0, 0.20000000000000001, 16.669444444444441,
     713.47500000000002, 7.3141280864197551}, /* small_e */
};
const int kNExpected = (int)(sizeof kExpected / sizeof kExpected[0]);

void run(const Spec& sp, Stats* st) {
  Board b;
  build(sp, &b);
  Analysis a;
  SolverScratch* sc = solver_scratch_create();
  solver_analyze(&b, &a, sc);
  solver_scratch_destroy(sc);
  measure(&b, &a, st);
}

}  // namespace

TEST(Golden, MatchesBaseline) {
  if (getenv("GOLDEN_CAPTURE")) {
    printf("const Stats kExpected[] = {\n");
    for (int i = 0; i < kNCorpus; ++i) {
      Stats st;
      run(kCorpus[i], &st);
      printf(
          "    {%d, %d, %d, %d, %d, %d, %.17g, %.17g, %.17g, %.17g, %.17g},  "
          "/* %s */\n",
          st.eval, st.best_x, st.best_y, st.interior_count, st.n_forced_safe,
          st.n_forced_mine, st.best_prob, st.interior_prob, st.sum_p, st.sum_ip,
          st.sum_p2, kCorpus[i].name);
    }
    printf("};\n");
    GTEST_SKIP() << "capture mode: paste the printed block into kExpected[]";
  }

  ASSERT_EQ(kNExpected, kNCorpus)
      << "baseline not captured; run GOLDEN_CAPTURE=1";
  for (int i = 0; i < kNCorpus; ++i) {
    Stats st;
    run(kCorpus[i], &st);
    const Stats& e = kExpected[i];
    /* tolerance: exact data-move (step 1) stays <1e-12; extraction (step 2)
     * may shift long double low bits, still far under the 1e-6 oracle. */
    double tol = 1e-9;
    SCOPED_TRACE(kCorpus[i].name);
    EXPECT_EQ(st.eval, e.eval);
    EXPECT_EQ(st.best_x, e.best_x);
    EXPECT_EQ(st.best_y, e.best_y);
    EXPECT_EQ(st.interior_count, e.interior_count);
    EXPECT_EQ(st.n_forced_safe, e.n_forced_safe);
    EXPECT_EQ(st.n_forced_mine, e.n_forced_mine);
    EXPECT_NEAR(st.best_prob, e.best_prob, tol);
    EXPECT_NEAR(st.interior_prob, e.interior_prob, tol);
    EXPECT_NEAR(st.sum_p, e.sum_p, tol * (1.0 + st.sum_p));
    EXPECT_NEAR(st.sum_ip, e.sum_ip, tol * (1.0 + st.sum_ip));
    EXPECT_NEAR(st.sum_p2, e.sum_p2, tol * (1.0 + st.sum_p2));
  }
}
