/* engine_infogain_test.cc — verifies solver_analyze_infogain's Inf(x) against
 * an independent brute-force reference.
 *
 * Inf(x) = the number of OTHER frontier cells that become provably safe or mine
 * if cell x is assumed safe (the paper's invariable count after substituting
 * x=0). The reference enumerates EVERY {0,1} assignment to the frontier that
 * satisfies the revealed-number constraints (no global mine budget — Inf(x) is
 * Phi-local), then counts cells that go from uncertain to fixed once the
 * candidate is pinned safe. Boards are pure guesses (no single-point deduction,
 * no forced cell) so the engine's per-component view equals the whole-frontier
 * reference.
 */
#include <gtest/gtest.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "engine_internal.h"
#include "minesweeper/game.h"
#include "solver/engine.h"

namespace {

/* ---- dense-board corpus (for the reduced-path differential below) ---------
 * Mirrors engine_reduction_test's generator: place mines, reveal each non-mine
 * with reveal_pct%, self-consistent adjacents. Low reveal_pct stresses dense
 * frontier components (which straddle CAP_VARS and exercise the reduction). */
enum { DENSE_GMAX = 30 * 16 };

uint32_t dense_next(uint32_t* s) {
  *s = *s * 1103515245u + 12345u;
  return *s;
}

int dense_adjacent(const unsigned char* mine, int w, int h, int x, int y) {
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

void build_dense(int w, int h, int mines, uint32_t seed, int reveal_pct,
                 struct Board* b) {
  memset(b, 0, sizeof *b);
  b->width = w;
  b->height = h;
  b->status = GAME_PLAYING;
  b->mines_placed = true;
  int ncell = w * h;
  unsigned char mine[DENSE_GMAX];
  memset(mine, 0, sizeof mine);
  uint32_t s = seed;
  int placed = 0;
  while (placed < mines) {
    int p = (int)((dense_next(&s) >> 8) % (uint32_t)ncell);
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
      if ((int)((dense_next(&s) >> 8) % 100u) < reveal_pct) {
        b->cells[i].revealed = true;
        b->cells[i].adjacent = (uint8_t)dense_adjacent(mine, w, h, x, y);
        ++revealed;
      }
    }
  }
  b->mines = mines;
  b->revealed_count = revealed;
}

/* Build a board from ASCII rows: '*' = mine (covered), '#' = safe covered,
 * '.' = safe revealed. Adjacent counts are derived from the mine layout. */
void build_ascii(struct Board* b, const char* const* rows, int h) {
  int w = (int)strlen(rows[0]);
  memset(b, 0, sizeof *b);
  b->width = w;
  b->height = h;
  b->status = GAME_PLAYING;
  int mines = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int i = game_index(b, x, y);
      b->cells[i].mine = (rows[y][x] == '*');
      if (b->cells[i].mine) ++mines;
    }
  }
  b->mines = mines;
  int rc = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int i = game_index(b, x, y);
      int adj = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          int nx = x + dx;
          int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          if (b->cells[game_index(b, nx, ny)].mine) ++adj;
        }
      }
      b->cells[i].adjacent = adj;
      if (rows[y][x] == '.') {
        b->cells[i].revealed = true;
        ++rc;
      }
    }
  }
  b->revealed_count = rc;
}

/* Independent frontier model: covered cells adjacent to a revealed numbered
 * (adjacent>0) cell are variables; each such revealed cell is a constraint over
 * its covered neighbors with need == adjacent. Mirrors the engine's
 * build_constraints. */
struct RefModel {
  int nvar;
  int var_of_cell[BOARD_MAX_CELLS];
  int cell_of_var[64];
  int ncon;
  int con_var[256][8];
  int con_nv[256];
  int con_need[256];
};

void ref_build(const struct Board* b, RefModel* m) {
  memset(m, 0, sizeof *m);
  for (int i = 0; i < b->width * b->height; ++i) m->var_of_cell[i] = -1;
  for (int y = 0; y < b->height; ++y) {
    for (int x = 0; x < b->width; ++x) {
      const struct Cell* c = &b->cells[game_index(b, x, y)];
      if (!c->revealed || c->mine || c->adjacent == 0) continue;
      int vars[8];
      int nv = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          int nx = x + dx;
          int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= b->width || ny >= b->height) continue;
          int idx = game_index(b, nx, ny);
          if (b->cells[idx].revealed) continue;
          if (m->var_of_cell[idx] < 0) {
            m->var_of_cell[idx] = m->nvar;
            m->cell_of_var[m->nvar] = idx;
            ++m->nvar;
          }
          vars[nv++] = m->var_of_cell[idx];
        }
      }
      if (nv == 0) continue;
      for (int j = 0; j < nv; ++j) m->con_var[m->ncon][j] = vars[j];
      m->con_nv[m->ncon] = nv;
      m->con_need[m->ncon] = c->adjacent;
      ++m->ncon;
    }
  }
}

bool ref_satisfies(const RefModel* m, const int* asg) {
  for (int ci = 0; ci < m->ncon; ++ci) {
    int s = 0;
    for (int j = 0; j < m->con_nv[ci]; ++j) s += asg[m->con_var[ci][j]];
    if (s != m->con_need[ci]) return false;
  }
  return true;
}

/* Brute-force Inf(x) for the candidate cell: enumerate all solutions, track per
 * var whether it is fixed across ALL solutions (orig) and across solutions with
 * the candidate safe (restricted); count vars (other than the candidate) that
 * become fixed only once the candidate is pinned safe. */
int ref_infogain(const struct Board* b, int cand_cell) {
  RefModel m;
  ref_build(b, &m);
  int cv = m.var_of_cell[cand_cell];
  if (cv < 0) return 0;
  int n = m.nvar;
  bool seen0[64], seen1[64], rseen0[64], rseen1[64];
  for (int v = 0; v < n; ++v) {
    seen0[v] = seen1[v] = rseen0[v] = rseen1[v] = false;
  }
  int asg[64];
  long long combos = 1LL << n;
  for (long long mask = 0; mask < combos; ++mask) {
    for (int v = 0; v < n; ++v) asg[v] = (int)((mask >> v) & 1);
    if (!ref_satisfies(&m, asg)) continue;
    for (int v = 0; v < n; ++v) {
      if (asg[v])
        seen1[v] = true;
      else
        seen0[v] = true;
    }
    if (asg[cv] == 0) {
      for (int v = 0; v < n; ++v) {
        if (asg[v])
          rseen1[v] = true;
        else
          rseen0[v] = true;
      }
    }
  }
  int gain = 0;
  for (int v = 0; v < n; ++v) {
    if (v == cv) continue;
    bool orig_inv = !(seen0[v] && seen1[v]);
    bool restr_reached = rseen0[v] || rseen1[v];
    bool restr_inv = restr_reached && !(rseen0[v] && rseen1[v]);
    if (restr_inv && !orig_inv) ++gain;
  }
  return gain;
}

/* Assert the engine's info_gain matches the reference for every covered,
 * non-forced frontier cell, and return how many cells had a positive gain. */
int check_against_ref(const struct Board* b, const struct Analysis* a) {
  int positives = 0;
  for (int i = 0; i < b->width * b->height; ++i) {
    const struct CellAnalysis* ca = &a->cells[i];
    if (b->cells[i].revealed) {
      EXPECT_EQ(ca->info_gain, 0) << "revealed cell " << i;
      continue;
    }
    if (!ca->is_frontier || ca->forced_mine) {
      EXPECT_EQ(ca->info_gain, 0) << "non-frontier/forced cell " << i;
      continue;
    }
    int ref = ref_infogain(b, i);
    EXPECT_EQ(ca->info_gain, ref) << "frontier cell " << i;
    if (ca->info_gain > 0) ++positives;
  }
  return positives;
}

struct Fixture {
  struct Board b;
  struct Analysis a;
  struct SolverScratch* s;
  Fixture() { this->s = solver_scratch_create(); }
  ~Fixture() { solver_scratch_destroy(this->s); }
};

}  // namespace

/* A+B=1 (single "1" witnessed by two safe cells): pinning either safe forces
 * the other to be a mine. Inf == 1 for both. */
TEST(EngineInfogain, PairForcesPartner) {
  Fixture f;
  const char* rows[] = {"#*", ".."};
  build_ascii(&f.b, rows, 2);
  solver_analyze_infogain(&f.b, &f.a, f.s);
  ASSERT_EQ(f.a.eval, EVAL_GUESS);
  EXPECT_EQ(f.a.cells[game_index(&f.b, 0, 0)].info_gain, 1);
  EXPECT_EQ(f.a.cells[game_index(&f.b, 1, 0)].info_gain, 1);
  EXPECT_EQ(check_against_ref(&f.b, &f.a), 2);
}

/* A+B+C=2 (a "2" seeing exactly three covered cells): pinning any one safe
 * forces the other two to be mines. Inf == 2 for all three. */
TEST(EngineInfogain, TripleTwoForcesPair) {
  Fixture f;
  const char* rows[] = {".*", "*#"};
  build_ascii(&f.b, rows, 2);
  solver_analyze_infogain(&f.b, &f.a, f.s);
  ASSERT_EQ(f.a.eval, EVAL_GUESS);
  EXPECT_EQ(f.a.cells[game_index(&f.b, 1, 0)].info_gain, 2);
  EXPECT_EQ(f.a.cells[game_index(&f.b, 0, 1)].info_gain, 2);
  EXPECT_EQ(f.a.cells[game_index(&f.b, 1, 1)].info_gain, 2);
  EXPECT_EQ(check_against_ref(&f.b, &f.a), 3);
}

/* A+B+C=1: a safe guess removes one unit of risk but forces nothing. Inf == 0.
 */
TEST(EngineInfogain, NonForcingGuessIsZero) {
  Fixture f;
  const char* rows[] = {".#", "#*"};
  build_ascii(&f.b, rows, 2);
  solver_analyze_infogain(&f.b, &f.a, f.s);
  ASSERT_EQ(f.a.eval, EVAL_GUESS);
  EXPECT_EQ(check_against_ref(&f.b, &f.a), 0);
}

/* Independent components: pinning a cell only forces within its own component.
 */
TEST(EngineInfogain, IndependentComponents) {
  Fixture f;
  const char* rows[] = {"#*", "..", "..", "..", "#*"};
  build_ascii(&f.b, rows, 5);
  solver_analyze_infogain(&f.b, &f.a, f.s);
  ASSERT_EQ(f.a.eval, EVAL_GUESS);
  /* each top/bottom pair forces only its partner: gain 1, never 2 */
  EXPECT_EQ(f.a.cells[game_index(&f.b, 0, 0)].info_gain, 1);
  EXPECT_EQ(f.a.cells[game_index(&f.b, 0, 4)].info_gain, 1);
  EXPECT_EQ(check_against_ref(&f.b, &f.a), 4);
}

/* When a forced-safe move exists (EVAL_SAFE, not a guess), info_gain is never
 * computed — the policy will just take the free safe cell. */
TEST(EngineInfogain, NotComputedWhenNotGuessing) {
  Fixture f;
  /* No mines: the lone covered cell (1,0) is interior with P=0 -> forced safe
   * -> EVAL_SAFE, so info_gain is never computed. */
  const char* rows[] = {".#", ".."};
  build_ascii(&f.b, rows, 2);
  solver_analyze_infogain(&f.b, &f.a, f.s);
  for (int i = 0; i < f.b.width * f.b.height; ++i) {
    EXPECT_EQ(f.a.cells[i].info_gain, 0) << "cell " << i;
  }
}

TEST(EngineInfogain, Deterministic) {
  Fixture f;
  const char* rows[] = {".*", "*#"};
  build_ascii(&f.b, rows, 2);
  struct Analysis a2;
  solver_analyze_infogain(&f.b, &f.a, f.s);
  solver_analyze_infogain(&f.b, &a2, f.s);
  for (int i = 0; i < f.b.width * f.b.height; ++i) {
    EXPECT_EQ(f.a.cells[i].info_gain, a2.cells[i].info_gain) << "cell " << i;
  }
}

/* Differential guard for component_infogain's REDUCED path. The ASCII fixtures
 * above only reach the direct path (tiny components); this drives the
 * Gaussian-reduction path on a dense corpus via the force-reduce hook and
 * asserts info_gain is byte-identical to the trusted direct enumeration.
 * Chained with the brute-force-oracle tests (direct == oracle on small boards),
 * this proves reduced == direct == oracle for Inf(x). Must hold before AND
 * after the build_local_model/enumerate_local extraction. */
TEST(EngineInfogain, DifferentialReducedVsDirect) {
  SolverScratch* sc = solver_scratch_create();
  Analysis* a1 = (Analysis*)malloc(sizeof *a1);
  Analysis* a2 = (Analysis*)malloc(sizeof *a2);
  int guess_boards = 0;
  for (uint32_t seed = 1; seed <= 80; ++seed) {
    for (int dens = 0; dens < 3; ++dens) {
      int mines = dens == 0 ? 30 : (dens == 1 ? 60 : 99);
      int pct = dens == 0 ? 65 : (dens == 1 ? 55 : 50);
      Board b;
      build_dense(30, 16, mines, seed * 7u + (uint32_t)dens, pct, &b);

      solver_test_set_force_reduce(sc, false); /* direct (trusted) */
      solver_analyze_infogain(&b, a1, sc);
      solver_test_set_force_reduce(sc, true); /* reduced (under test) */
      solver_analyze_infogain(&b, a2, sc);
      solver_test_set_force_reduce(sc, false);

      ASSERT_EQ(a1->eval, a2->eval) << "seed " << seed << " dens " << dens;
      for (int i = 0; i < b.width * b.height; ++i) {
        ASSERT_EQ(a1->cells[i].info_gain, a2->cells[i].info_gain)
            << "seed " << seed << " dens " << dens << " cell " << i;
      }
      if (a1->eval == EVAL_GUESS) ++guess_boards;
    }
  }
  EXPECT_GT(guess_boards, 0); /* corpus must actually exercise guessing */
  free(a1);
  free(a2);
  solver_scratch_destroy(sc);
}
