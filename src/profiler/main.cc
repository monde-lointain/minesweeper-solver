/* main.cc — bounded replay of the pathological solver run, for callgrind.
 *
 * Replays the exact workload that motivated profiling: a 100x100 board with
 * 2500 mines, seed 7, info-gain policy — identical to the bench harness's
 * play_one loop (src/bench/runner.cc), but truncated to a move cap so a
 * valgrind/callgrind run finishes in a few minutes instead of tens.
 *
 * The cap truncates only; it never changes the move sequence, so the profiled
 * code path is faithful to the real game. The whole workload is deterministic
 * (seed + mine placement + solver_recommend_move are all deterministic), so
 * repeated runs produce identical callgrind profiles — directly comparable
 * across optimisation iterations.
 *
 * Orthodox C++: POD, pointers, C headers, C-style casts, no auto/lambdas.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "minesweeper/game.h"  /* struct Board, game_reset/reveal, GameStatus */
#include "solver/engine.h"     /* solver_analyze_infogain, SolverScratch */
#include "solver/geom.h"       /* struct Pt */
#include "solver/recommend.h"  /* solver_recommend_move */

enum {
  PROF_W = 100,
  PROF_H = 100,
  PROF_MINES = 2500,
  PROF_SEED = 7,
  /* Tuned so `make profile` runs ~3 min under callgrind; override via argv[1]
   * (<=0 plays the full game). */
  PROF_DEFAULT_MAX_MOVES = 500
};

int main(int argc, char** argv) {
  int max_moves = PROF_DEFAULT_MAX_MOVES;
  if (argc > 1) {
    max_moves = atoi(argv[1]);
  }

  struct Board b;
  struct Rng rng;
  rng.fn = NULL;
  rng.ctx = NULL;
  rng.seed = (uint32_t)PROF_SEED;
  game_reset(&b, PROF_W, PROF_H, PROF_MINES, &rng);

  struct SolverScratch* sc = solver_scratch_create();
  struct Analysis* a = (struct Analysis*)malloc(sizeof *a);
  if (sc == NULL || a == NULL) {
    fprintf(stderr, "profiler: out of memory\n");
    solver_scratch_destroy(sc);
    free(a);
    return 1;
  }

  int moves = 0;
  while (b.status == GAME_READY || b.status == GAME_PLAYING) {
    if (max_moves > 0 && moves >= max_moves) {
      break;
    }
    solver_analyze_infogain(&b, a, sc);

    struct Pt mv;
    if (solver_recommend_move(&b, a, &mv) != 0) {
      break; /* no covered cell — terminal */
    }
    game_reveal(&b, mv.x, mv.y);
    ++moves;
  }

  printf("profiler: moves=%d status=%d revealed=%d/%d\n", moves, b.status,
         b.revealed_count, PROF_W * PROF_H - PROF_MINES);

  free(a);
  solver_scratch_destroy(sc);
  return 0;
}
