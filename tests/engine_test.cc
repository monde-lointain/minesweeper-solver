/* engine_test.cc — probability engine tests (Stream B).
 *
 * The oracle is a brute-force reference: enumerate every placement of `mines`
 * mines over the covered cells consistent with the revealed numbers, and take
 * exact marginals. The engine's mine_prob must match for every covered cell.
 */
#include "solver/engine.h"

#include <gtest/gtest.h>
#include <string.h>

#include "minesweeper/game.h"

namespace {

constexpr int kMaxC = 64; /* tests use boards up to 8x8 */

static int popcount_u64(unsigned long long x) {
  int c = 0;
  while (x) { x &= x - 1; ++c; }
  return c;
}

struct TB {
  int w;
  int h;
  bool mine[kMaxC]; /* ground-truth mine layout */
  bool
      revealed[kMaxC]; /* which (non-mine) cells are revealed (numbered only) */
};

int ix(int w, int x, int y) { return y * w + x; }

int true_adjacent(const TB& tb, int x, int y) {
  int n = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) continue;
      int nx = x + dx;
      int ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= tb.w || ny >= tb.h) continue;
      if (tb.mine[ix(tb.w, nx, ny)]) ++n;
    }
  }
  return n;
}

void build_board(const TB& tb, Board* b) {
  memset(b, 0, sizeof *b);
  b->width = tb.w;
  b->height = tb.h;
  b->status = GAME_PLAYING;
  b->mines_placed = true;
  int mines = 0;
  int revealed = 0;
  for (int y = 0; y < tb.h; ++y) {
    for (int x = 0; x < tb.w; ++x) {
      int i = ix(tb.w, x, y);
      Cell* c = &b->cells[i];
      c->mine = tb.mine[i];
      if (tb.mine[i]) ++mines;
      if (tb.revealed[i]) {
        c->revealed = true;
        c->adjacent = (uint8_t)true_adjacent(tb, x, y);
        ++revealed;
      }
    }
  }
  b->mines = mines;
  b->revealed_count = revealed;
}

/* Brute-force exact marginals over covered cells. Returns #solutions. */
long long brute(const TB& tb, double* prob) {
  int covered[kMaxC];
  int nc = 0;
  for (int y = 0; y < tb.h; ++y)
    for (int x = 0; x < tb.w; ++x)
      if (!tb.revealed[ix(tb.w, x, y)]) covered[nc++] = ix(tb.w, x, y);

  int total_mines = 0;
  for (int i = 0; i < tb.w * tb.h; ++i)
    if (tb.mine[i]) ++total_mines;

  /* precompute revealed numbered cells + their target counts */
  int rcell[kMaxC];
  int rneed[kMaxC];
  int nr = 0;
  for (int y = 0; y < tb.h; ++y) {
    for (int x = 0; x < tb.w; ++x) {
      int i = ix(tb.w, x, y);
      if (tb.revealed[i]) {
        rcell[nr] = i;
        rneed[nr] = true_adjacent(tb, x, y);
        ++nr;
      }
    }
  }

  long long nsol = 0;
  long long cnt[kMaxC];
  for (int j = 0; j < nc; ++j) cnt[j] = 0;

  bool placed[kMaxC];
  for (unsigned long long mask = 0; mask < (1ULL << nc); ++mask) {
    if (popcount_u64(mask) != total_mines) continue;
    for (int i = 0; i < tb.w * tb.h; ++i) placed[i] = false;
    for (int j = 0; j < nc; ++j)
      if (mask & (1ULL << j)) placed[covered[j]] = true;

    bool ok = true;
    for (int r = 0; r < nr && ok; ++r) {
      int cx = rcell[r] % tb.w;
      int cy = rcell[r] / tb.w;
      int m = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          int nx = cx + dx;
          int ny = cy + dy;
          if (nx < 0 || ny < 0 || nx >= tb.w || ny >= tb.h) continue;
          if (placed[ix(tb.w, nx, ny)]) ++m;
        }
      if (m != rneed[r]) ok = false;
    }
    if (!ok) continue;
    ++nsol;
    for (int j = 0; j < nc; ++j)
      if (mask & (1ULL << j)) ++cnt[j];
  }

  for (int i = 0; i < tb.w * tb.h; ++i) prob[i] = 0.0;
  if (nsol > 0)
    for (int j = 0; j < nc; ++j)
      prob[covered[j]] = (double)cnt[j] / (double)nsol;
  return nsol;
}

/* Build, analyze, and assert engine == brute for every covered cell. */
void expect_matches_brute(const TB& tb, SolverScratch* s) {
  Board b;
  build_board(tb, &b);
  Analysis a;
  solver_analyze(&b, &a, s);

  double ref[kMaxC];
  long long nsol = brute(tb, ref);
  ASSERT_GT(nsol, 0) << "scenario has no consistent placement";

  double sum = 0.0;
  for (int i = 0; i < tb.w * tb.h; ++i) {
    if (b.cells[i].revealed) continue;
    EXPECT_NEAR(a.cells[i].mine_prob, ref[i], 1e-6)
        << "cell " << i << " (x=" << i % tb.w << ",y=" << i / tb.w << ")";
    sum += a.cells[i].mine_prob;
  }
  /* invariant: expected total mines among covered == mines */
  EXPECT_NEAR(sum, (double)b.mines, 1e-6);
}

}  // namespace

/* Fixture: one heap SolverScratch per test (reuse within a test is safe). */
class Engine : public ::testing::Test {
 protected:
  void SetUp() override {
    s = solver_scratch_create();
    ASSERT_NE(s, nullptr);
  }
  void TearDown() override { solver_scratch_destroy(s); }
  SolverScratch* s = nullptr;
};

TEST_F(Engine, CornerOneMine) {
  /* 2x2, mine at (1,1), reveal (0,0)="1" -> each covered cell P=1/3. */
  TB tb;
  memset(&tb, 0, sizeof tb);
  tb.w = 2;
  tb.h = 2;
  tb.mine[ix(2, 1, 1)] = true;
  tb.revealed[ix(2, 0, 0)] = true;
  expect_matches_brute(tb, s);

  Board b;
  build_board(tb, &b);
  Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_NEAR(a.cells[ix(2, 1, 0)].mine_prob, 1.0 / 3.0, 1e-6);
  EXPECT_EQ(a.eval, EVAL_GUESS);
  EXPECT_NEAR(a.best_prob, 1.0 / 3.0, 1e-6);
}

TEST_F(Engine, OneTwoOneForced) {
  /* top row 1,2,1 over a 3-wide covered bottom row -> mines at (0,1),(2,1),
   * (1,1) safe. Unique solution: deterministic. */
  TB tb;
  memset(&tb, 0, sizeof tb);
  tb.w = 3;
  tb.h = 2;
  tb.mine[ix(3, 0, 1)] = true;
  tb.mine[ix(3, 2, 1)] = true;
  tb.revealed[ix(3, 0, 0)] = true;
  tb.revealed[ix(3, 1, 0)] = true;
  tb.revealed[ix(3, 2, 0)] = true;
  expect_matches_brute(tb, s);

  Board b;
  build_board(tb, &b);
  Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_TRUE(a.cells[ix(3, 0, 1)].forced_mine);
  EXPECT_TRUE(a.cells[ix(3, 2, 1)].forced_mine);
  EXPECT_TRUE(a.cells[ix(3, 1, 1)].forced_safe);
  EXPECT_EQ(a.eval, EVAL_SAFE);
  EXPECT_EQ(a.best.x, 1);
  EXPECT_EQ(a.best.y, 1);
  EXPECT_NEAR(a.best_prob, 0.0, 1e-9);
}

TEST_F(Engine, SinglePointForcedMine) {
  /* 1x2 column: reveal (0,0)="1" with one covered neighbor (0,1) -> mine. */
  TB tb;
  memset(&tb, 0, sizeof tb);
  tb.w = 1;
  tb.h = 2;
  tb.mine[ix(1, 0, 1)] = true;
  tb.revealed[ix(1, 0, 0)] = true;
  expect_matches_brute(tb, s);

  Board b;
  build_board(tb, &b);
  Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_TRUE(a.cells[ix(1, 0, 1)].forced_mine);
}

TEST_F(Engine, InteriorCellsShareRemainder) {
  /* 4x4, 3 mines. Reveal a numbered cell touching one corner; far cells are
   * interior and share the remaining mine budget. Engine must match brute. */
  TB tb;
  memset(&tb, 0, sizeof tb);
  tb.w = 4;
  tb.h = 4;
  tb.mine[ix(4, 1, 0)] = true; /* near the revealed cell */
  tb.mine[ix(4, 3, 3)] = true; /* interior-ish */
  tb.mine[ix(4, 0, 3)] = true;
  tb.revealed[ix(4, 0, 0)] = true; /* "1": neighbors (1,0),(0,1),(1,1) */
  expect_matches_brute(tb, s);

  Board b;
  build_board(tb, &b);
  Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_GT(a.interior_count, 0);
}

TEST_F(Engine, RandomBoardsMatchBrute) {
  /* deterministic pseudo-random small boards; compare engine to brute. */
  unsigned int seed = 0x1234567u;
  for (int iter = 0; iter < 40; ++iter) {
    TB tb;
    memset(&tb, 0, sizeof tb);
    tb.w = 4;
    tb.h = 4;
    int ncell = tb.w * tb.h;
    int want_mines = 2 + (iter % 4); /* 2..5 mines */
    /* place mines */
    int placed = 0;
    while (placed < want_mines) {
      seed = seed * 1103515245u + 12345u;
      int p = (int)((seed >> 8) % (unsigned)ncell);
      if (!tb.mine[p]) {
        tb.mine[p] = true;
        ++placed;
      }
    }
    /* reveal a random subset of numbered (adjacent>0) non-mine cells */
    int revealed = 0;
    for (int y = 0; y < tb.h; ++y) {
      for (int x = 0; x < tb.w; ++x) {
        int i = ix(tb.w, x, y);
        if (tb.mine[i]) continue;
        if (true_adjacent(tb, x, y) == 0) continue;
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) & 1u) {
          tb.revealed[i] = true;
          ++revealed;
        }
      }
    }
    if (revealed == 0) continue; /* nothing to constrain; skip */
    /* keep covered count small enough for brute force */
    int covered = ncell - revealed;
    if (covered > 16) continue;
    expect_matches_brute(tb, s);
  }
}

TEST_F(Engine, StartUniform) {
  Board b;
  memset(&b, 0, sizeof b);
  b.width = 9;
  b.height = 9;
  b.mines = 10;
  b.status = GAME_READY;
  Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_EQ(a.eval, EVAL_START);
  EXPECT_NEAR(a.cells[0].mine_prob, 10.0 / 81.0, 1e-9);
  EXPECT_EQ(a.best.x, 0);
  EXPECT_EQ(a.best.y, 0);
}

TEST_F(Engine, WonAndLost) {
  Board b;
  memset(&b, 0, sizeof b);
  b.width = 9;
  b.height = 9;
  b.mines = 10;
  b.revealed_count = 5;
  Analysis a;
  b.status = GAME_WON;
  solver_analyze(&b, &a, s);
  EXPECT_EQ(a.eval, EVAL_SOLVED);
  b.status = GAME_LOST;
  solver_analyze(&b, &a, s);
  EXPECT_EQ(a.eval, EVAL_LOST);
}

/* Reentrancy: two boards on two independent SolverScratch handles. Interleaving
 * an analysis of board B between analyses of board A must not perturb A — i.e.
 * the engine's output depends only on (board, its own scratch), so parallel
 * solver instances are safe. Would fail if any mutable engine state were
 * shared between handles. */
TEST_F(Engine, ReentrantInterleaved) {
  TB ta; /* 2x2: mine at (1,1), reveal (0,0) */
  memset(&ta, 0, sizeof ta);
  ta.w = 2;
  ta.h = 2;
  ta.mine[ix(2, 1, 1)] = true;
  ta.revealed[ix(2, 0, 0)] = true;

  TB tb; /* 4x4, 3 mines, one revealed corner */
  memset(&tb, 0, sizeof tb);
  tb.w = 4;
  tb.h = 4;
  tb.mine[ix(4, 1, 0)] = true;
  tb.mine[ix(4, 3, 3)] = true;
  tb.mine[ix(4, 0, 3)] = true;
  tb.revealed[ix(4, 0, 0)] = true;

  Board ba;
  Board bb;
  build_board(ta, &ba);
  build_board(tb, &bb);

  SolverScratch* sa = solver_scratch_create();
  SolverScratch* sb = solver_scratch_create();
  ASSERT_NE(sa, nullptr);
  ASSERT_NE(sb, nullptr);

  Analysis refA;
  Analysis refB;
  solver_analyze(&ba, &refA, sa);
  solver_analyze(&bb, &refB, sb);

  for (int iter = 0; iter < 5; ++iter) {
    Analysis a2;
    Analysis b2;
    solver_analyze(&bb, &b2, sb); /* B between A's re-analyses */
    solver_analyze(&ba, &a2, sa);
    for (int i = 0; i < ba.width * ba.height; ++i) {
      EXPECT_EQ(a2.cells[i].mine_prob, refA.cells[i].mine_prob);
    }
    for (int i = 0; i < bb.width * bb.height; ++i) {
      EXPECT_EQ(b2.cells[i].mine_prob, refB.cells[i].mine_prob);
    }
  }

  solver_scratch_destroy(sa);
  solver_scratch_destroy(sb);
}
