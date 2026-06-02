/* engine_emulated_test.cc — emulated-128-bit parity (MSVC path coverage).
 *
 * The exact-rational reduction path does all 128-bit arithmetic through RatI128
 * (rational.h). On GCC/Clang that is the native __int128; on MSVC it is the
 * two-limb software emulation (RAT_FORCE_EMULATED_I128) — a path CI otherwise
 * never compiles or runs. solver_lib_emu recompiles the SAME engine sources
 * with that emulation forced and public symbols renamed `_emu` (see
 * CMakeLists.txt), so here the production (native-i128) engine and the emulated
 * engine analyze the same boards and must agree EXACTLY: both evaluate
 * identical exact rationals, so every marginal is bit-identical.
 *
 * force_reduce is set on both engines so every component — not just the dense
 * >CAP_VARS ones — is driven through the reduced path that exercises RatI128.
 */
#include <gtest/gtest.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "engine_internal.h" /* solver_test_set_force_reduce (native hook) */
#include "minesweeper/game.h"
#include "solver/engine.h"

/* Emulated-i128 engine (renamed symbols from solver_lib_emu). */
struct SolverScratch* solver_scratch_create_emu(void);
void solver_scratch_destroy_emu(struct SolverScratch* s);
void solver_analyze_emu(const struct Board* b, struct Analysis* out,
                        struct SolverScratch* s);
void solver_test_set_force_reduce_emu(struct SolverScratch* s, bool on);

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

/* Deterministic dense board: place `mines`, reveal each non-mine with
 * `reveal_pct`% probability. Low reveal_pct -> dense components that stress the
 * reduction path. (Mirrors engine_reduction_test's builder.) */
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

/* Native-i128 and emulated-i128 reduced enumeration must agree bit-for-bit. */
TEST(EngineEmulated, NativeVsEmulatedReducedAgree) {
  SolverScratch* sn = solver_scratch_create();
  SolverScratch* se = solver_scratch_create_emu();
  Analysis* an = (Analysis*)malloc(sizeof *an);
  Analysis* ae = (Analysis*)malloc(sizeof *ae);
  ASSERT_NE(sn, nullptr);
  ASSERT_NE(se, nullptr);
  ASSERT_NE(an, nullptr);
  ASSERT_NE(ae, nullptr);

  solver_test_set_force_reduce(sn, true);
  solver_test_set_force_reduce_emu(se, true);

  int boards = 0;
  for (uint32_t seed = 1; seed <= 120; ++seed) {
    for (int dens = 0; dens < 3; ++dens) {
      int mines = dens == 0 ? 30 : (dens == 1 ? 60 : 99);
      int pct = dens == 0 ? 65 : (dens == 1 ? 55 : 50);
      Board b;
      build(30, 16, mines, seed * 7u + (uint32_t)dens, pct, &b);

      solver_analyze(&b, an, sn);
      solver_analyze_emu(&b, ae, se);

      ASSERT_EQ(an->eval, ae->eval) << "seed " << seed << " dens " << dens;
      for (int i = 0; i < b.width * b.height; ++i) {
        if (b.cells[i].revealed) continue;
        /* identical exact rationals -> identical doubles */
        ASSERT_DOUBLE_EQ(an->cells[i].mine_prob, ae->cells[i].mine_prob)
            << "seed " << seed << " dens " << dens << " cell " << i;
        ASSERT_EQ(an->cells[i].forced_safe, ae->cells[i].forced_safe);
        ASSERT_EQ(an->cells[i].forced_mine, ae->cells[i].forced_mine);
      }
      ++boards;
    }
  }
  EXPECT_GT(boards, 0);

  solver_test_set_force_reduce(sn, false);
  solver_test_set_force_reduce_emu(se, false);
  free(an);
  free(ae);
  solver_scratch_destroy(sn);
  solver_scratch_destroy_emu(se);
}
