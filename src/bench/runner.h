/* runner.h — benchmark play loop + threaded fan-out.
 *
 * Plays `games` boards through the engine + a policy, accumulating Metrics. Each
 * game is fully determined by its seed (cfg->seed + game index), so the result
 * is deterministic for a given (seed, games, dims) regardless of thread count:
 * workers own private SolverScratch + Metrics with no shared mutable state, and
 * the per-worker Metrics merge commutatively.
 *
 * Orthodox C++ with a sanctioned, runner.cc-local exception for std::thread /
 * std::atomic (see CLAUDE.md heresy-suppression convention).
 */
#ifndef SOLVER_BENCH_RUNNER_H
#define SOLVER_BENCH_RUNNER_H

#include <stdbool.h>
#include <stdint.h>

#include "metrics.h"
#include "policy.h"

struct BenchConfig {
  int width;
  int height;
  int mines;
  uint64_t games;
  uint32_t seed;  /* base seed; game i uses seed + i */
  int threads;    /* <= 0 means auto (hardware concurrency) */
  int policy_id;  /* enum PolicyId */
  bool quiet;
  char label[32]; /* difficulty label for output */
};

/* Run the benchmark per cfg, writing aggregate metrics to *out. Returns the
 * resolved worker-thread count actually used. */
int bench_run(const struct BenchConfig* cfg, struct Metrics* out);

#endif /* SOLVER_BENCH_RUNNER_H */
