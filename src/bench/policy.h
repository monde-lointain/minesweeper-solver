/* policy.h — move-selection seam for the benchmark harness.
 *
 * The engine (engine.h) produces per-cell P(mine); a policy turns that Analysis
 * into the next cell to reveal. POLICY_BASELINE reproduces the engine's own
 * lowest-mine-probability pick, so Phase-1 numbers are the current solver's true
 * winrate. Future strategies add enum cases without touching the runner.
 *
 * Orthodox C++: POD, plain enums, pointers, C headers.
 */
#ifndef SOLVER_BENCH_POLICY_H
#define SOLVER_BENCH_POLICY_H

#include "minesweeper/game.h"  /* struct Board, game_index */
#include "solver/engine.h"     /* struct Analysis */

/* A covered cell to reveal. */
struct Move {
  int x;
  int y;
};

enum PolicyId {
  POLICY_INFOGAIN = 0,  /* min-prob + info-gain tie-break (paper's Inf(x)); default */
  POLICY_BASELINE = 1,  /* engine min-prob pick — the engine-accuracy reference */
  POLICY_COUNT = 2
};

/* True if the policy needs the engine's info_gain field populated (so the runner
 * calls solver_analyze_infogain instead of the cheaper solver_analyze). */
int policy_needs_infogain(int policy_id);

/* Choose a covered cell to reveal for the given analysis. Returns 0 and writes
 * *out on success; returns -1 if no covered cell exists. Pure: reads b and a,
 * writes only *out. */
int policy_select(int policy_id, const struct Board* b,
                  const struct Analysis* a, struct Move* out);

#endif /* SOLVER_BENCH_POLICY_H */
