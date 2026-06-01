/* engine_exact_test.cc — Analysis.exact: true when whole-board probabilities
 * are proven (no component fell back), false when any component is approximate.
 */
#include <gtest/gtest.h>
#include <string.h>

#include "minesweeper/game.h"
#include "solver/engine.h"

namespace {

/* An ambiguous covered strip: row 1 revealed; rows 0/2 covered with mines at
 * (even,0) and (odd,2). Every middle constraint stays loose (rem 2..3 over 4..6
 * unknowns) so single-point deduction resolves nothing — the whole covered set
 * is ONE undetermined component of 2*W vars. W=10 -> 20 vars (exact path);
 * W=40 -> 80 vars > MAX_COMP_VARS(64) -> fallback_component -> exact == false.
 */
void mk_ambiguous_strip(struct Board* b, int W) {
  memset(b, 0, sizeof *b);
  b->width = W;
  b->height = 3;
  b->status = GAME_PLAYING;
  int mines = 0;
  for (int x = 0; x < W; ++x) {
    if (x % 2 == 0) {
      b->cells[game_index(b, x, 0)].mine = true;
      ++mines;
    }
    if (x % 2 == 1) {
      b->cells[game_index(b, x, 2)].mine = true;
      ++mines;
    }
  }
  b->mines = mines;
  int rev = 0;
  for (int x = 0; x < W; ++x) {
    int adj = 0;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        int nx = x + dx;
        int ny = 1 + dy;
        if (nx < 0 || ny < 0 || nx >= W || ny >= 3) continue;
        if (b->cells[game_index(b, nx, ny)].mine) ++adj;
      }
    }
    struct Cell* c = &b->cells[game_index(b, x, 1)];
    c->revealed = true;
    c->adjacent = (uint8_t)adj;
    ++rev;
  }
  b->revealed_count = rev;
}

}  // namespace

TEST(AnalysisExact, TrueOnSmallExactFrontier) {
  struct Board b;
  mk_ambiguous_strip(&b, 10);  // 20 vars -> exact enumeration
  struct SolverScratch* s = solver_scratch_create();
  ASSERT_NE(s, nullptr);
  struct Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_TRUE(a.exact);
  solver_scratch_destroy(s);
}

TEST(AnalysisExact, FalseWhenComponentFallsBack) {
  struct Board b;
  mk_ambiguous_strip(&b, 40);  // 80 vars > MAX_COMP_VARS -> fallback
  struct SolverScratch* s = solver_scratch_create();
  ASSERT_NE(s, nullptr);
  struct Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_FALSE(a.exact);
  solver_scratch_destroy(s);
}

TEST(AnalysisExact, TrueOnTerminalAndStart) {
  struct SolverScratch* s = solver_scratch_create();
  ASSERT_NE(s, nullptr);
  struct Analysis a;
  struct Board b;
  memset(&b, 0, sizeof b);
  b.width = 9;
  b.height = 9;
  b.mines = 10;
  b.status = GAME_PLAYING;
  b.revealed_count = 0;  // EVAL_START
  solver_analyze(&b, &a, s);
  EXPECT_TRUE(a.exact);
  b.status = GAME_WON;  // terminal
  solver_analyze(&b, &a, s);
  EXPECT_TRUE(a.exact);
  solver_scratch_destroy(s);
}
