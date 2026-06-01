/* runner.cc — benchmark play loop + threaded fan-out (Stream B.3).
 *
 * Each game is fully determined by its seed (cfg->seed + game index). Workers
 * pull game-index chunks from a shared atomic counter (dynamic load balancing),
 * each owning a private SolverScratch + Metrics slot — no shared mutable state
 * on the hot path. Slots merge commutatively after join, so the aggregate is
 * identical for any thread count.
 *
 * Orthodox C++ with a sanctioned, file-local exception: std::thread /
 * std::atomic for the worker pool (CLAUDE.md threading carve-out). No other
 * Modern C++.
 */
#include "runner.h"

#include <stdlib.h>
#include <time.h>

#include <atomic>
#include <thread>

#include "minesweeper/game.h"
#include "solver/engine.h"

enum { BENCH_MAX_THREADS = 256, BENCH_CHUNK = 128 };

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

  int want_infogain = policy_needs_infogain(cfg->policy_id);
  while (b.status == GAME_READY || b.status == GAME_PLAYING) {
    uint64_t t0 = now_ns();
    if (want_infogain) {
      solver_analyze_infogain(&b, a, sc);
    } else {
      solver_analyze(&b, a, sc);
    }
    metrics_record_analyze(out, now_ns() - t0);

    struct Pt mv;
    if (policy_select(cfg->policy_id, &b, a, &mv) != 0) {
      break;
    }
    int idx = game_index(&b, mv.x, mv.y);
    int eval = a->eval;
    double pred = a->cells[idx].mine_prob;
    bool fsafe = a->cells[idx].forced_safe;
    game_reveal(&b, mv.x, mv.y);
    bool was_mine = b.cells[idx].mine; /* harness oracle: scoring only */
    metrics_record_decision(out, eval, pred, was_mine, fsafe);
  }

  bool won = (b.status == GAME_WON);
  int denom = cfg->width * cfg->height - cfg->mines;
  double prog = (denom > 0) ? (double)b.revealed_count / (double)denom : 0.0;
  metrics_record_game(out, won, prog);
}

/* Worker: pull chunks of game indices until the range is exhausted. */
static void bench_worker(const struct BenchConfig* cfg,
                         std::atomic<uint64_t>* next, struct Metrics* slot) {
  metrics_zero(slot);
  struct SolverScratch* sc = solver_scratch_create();
  struct Analysis* a = (struct Analysis*)malloc(sizeof *a);
  if (sc == NULL || a == NULL) {
    solver_scratch_destroy(sc);
    free(a);
    return;
  }
  for (;;) {
    uint64_t start = next->fetch_add((uint64_t)BENCH_CHUNK);
    if (start >= cfg->games) {
      break;
    }
    uint64_t end = start + (uint64_t)BENCH_CHUNK;
    if (end > cfg->games) {
      end = cfg->games;
    }
    for (uint64_t i = start; i < end; ++i) {
      play_one(cfg, cfg->seed + (uint32_t)i, sc, a, slot);
    }
  }
  free(a);
  solver_scratch_destroy(sc);
}

int bench_run(const struct BenchConfig* cfg, struct Metrics* out) {
  metrics_zero(out);
  int nthreads = cfg->threads;
  if (nthreads <= 0) {
    unsigned int hc = std::thread::hardware_concurrency();
    nthreads = (hc == 0u) ? 1 : (int)hc;
  }
  if (nthreads > BENCH_MAX_THREADS) {
    nthreads = BENCH_MAX_THREADS;
  }
  if (cfg->games == 0) {
    return nthreads;
  }

  std::atomic<uint64_t> next(0);
  struct Metrics* slots =
      (struct Metrics*)malloc((size_t)nthreads * sizeof *slots);
  if (slots == NULL) {
    return 0;
  }
  std::thread ths[BENCH_MAX_THREADS];
  for (int w = 0; w < nthreads; ++w) {
    ths[w] = std::thread(bench_worker, cfg, &next, &slots[w]);
  }
  for (int w = 0; w < nthreads; ++w) {
    ths[w].join();
  }
  for (int w = 0; w < nthreads; ++w) {
    metrics_merge(out, &slots[w]);
  }
  free(slots);
  return nthreads;
}
