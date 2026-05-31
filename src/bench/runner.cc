/* runner.cc — STUB (Stream A). Replaced by Stream B.3.
 * Single-threaded play loop; ignores cfg->threads (always 1 worker). Real
 * std::thread/std::atomic fan-out lands in B.3. Pure C-style here so it passes
 * Orthodoxy unchanged.
 */
#include "runner.h"

#include <stdlib.h>
#include <time.h>

#include "minesweeper/game.h"
#include "solver/engine.h"

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Play one seeded game into `out`, reusing scratch `sc` and analysis `a`. */
static void play_one(const struct BenchConfig* cfg, uint32_t seed,
                     struct SolverScratch* sc, struct Analysis* a,
                     struct Metrics* out) {
  struct Board b;
  struct Rng rng;
  rng.fn = NULL;
  rng.ctx = NULL;
  rng.seed = seed;
  game_reset(&b, cfg->width, cfg->height, cfg->mines, &rng);

  while (b.status == GAME_READY || b.status == GAME_PLAYING) {
    uint64_t t0 = now_ns();
    solver_analyze(&b, a, sc);
    metrics_record_analyze(out, now_ns() - t0);

    struct Move mv;
    if (policy_select(cfg->policy_id, &b, a, &mv) != 0) {
      break;
    }
    int idx = game_index(&b, mv.x, mv.y);
    int eval = a->eval;
    double pred = a->cells[idx].mine_prob;
    bool fsafe = a->cells[idx].forced_safe;
    game_reveal(&b, mv.x, mv.y);
    bool was_mine = b.cells[idx].mine;
    metrics_record_decision(out, eval, pred, was_mine, fsafe);
  }

  bool won = (b.status == GAME_WON);
  int denom = cfg->width * cfg->height - cfg->mines;
  double prog = (denom > 0) ? (double)b.revealed_count / (double)denom : 0.0;
  metrics_record_game(out, won, prog);
}

int bench_run(const struct BenchConfig* cfg, struct Metrics* out) {
  metrics_zero(out);
  struct SolverScratch* sc = solver_scratch_create();
  struct Analysis* a = (struct Analysis*)malloc(sizeof *a);
  if (sc == NULL || a == NULL) {
    solver_scratch_destroy(sc);
    free(a);
    return 0;
  }
  for (uint64_t i = 0; i < cfg->games; ++i) {
    play_one(cfg, cfg->seed + (uint32_t)i, sc, a, out);
  }
  solver_scratch_destroy(sc);
  free(a);
  return 1;
}
