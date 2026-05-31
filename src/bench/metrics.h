/* metrics.h — benchmark metrics accumulation + reporting.
 *
 * All accumulation lives here (not the runner) so it is unit-testable in
 * isolation. Every field is a commutative reduction (sum / max), so per-worker
 * Metrics merge to the same aggregate regardless of thread count. The runner
 * feeds one event per decision, per game, and per analyze call.
 *
 * Orthodox C++: POD, plain enums, pointers, C headers.
 */
#ifndef SOLVER_BENCH_METRICS_H
#define SOLVER_BENCH_METRICS_H

#include <stdbool.h>
#include <stdint.h>

enum { METRICS_NBUCKET = 20 }; /* calibration bins over P(mine) in [0,1] */

struct Metrics {
  uint64_t games;
  uint64_t wins;

  /* decision counts by engine eval */
  uint64_t start_moves; /* EVAL_START (opening) — excluded from guess stats */
  uint64_t safe_moves;  /* EVAL_SAFE */
  uint64_t guess_moves; /* EVAL_GUESS */

  uint64_t guesses_survived;
  uint64_t guesses_fatal;
  long double guess_risk_sum; /* sum of accepted P(mine) over guesses */

  /* guess calibration: predicted P vs actual mine, bucketed */
  uint64_t cal_n[METRICS_NBUCKET];
  long double cal_pred_sum[METRICS_NBUCKET];
  uint64_t cal_mine[METRICS_NBUCKET];

  uint64_t deaths_on_forced_safe; /* must be 0; >0 ⇒ engine correctness bug */

  long double loss_progress_sum; /* sum over losses of revealed fraction */

  /* perf */
  uint64_t analyze_calls;
  uint64_t analyze_ns_sum;
  uint64_t analyze_ns_max;
};

/* Zero all fields. */
void metrics_zero(struct Metrics* m);

/* dst += src, field-wise (max for analyze_ns_max). Commutative + associative. */
void metrics_merge(struct Metrics* dst, const struct Metrics* src);

/* Record one decision: `eval` (enum SolverEval), `pred` = accepted P(mine) of
 * the chosen cell, `was_mine` = that cell's true mine status (read after the
 * reveal), `forced_safe` = engine deemed it proven-safe. */
void metrics_record_decision(struct Metrics* m, int eval, double pred,
                             bool was_mine, bool forced_safe);

/* Record one finished game. loss_progress = revealed_count/(cells-mines) at end;
 * ignored when won. */
void metrics_record_game(struct Metrics* m, bool won, double loss_progress);

/* Record one solver_analyze call duration in nanoseconds. */
void metrics_record_analyze(struct Metrics* m, uint64_t ns);

/* Wilson 95% score interval for k of n successes -> [*lo,*hi]; [0,0] if n==0. */
void metrics_wilson95(uint64_t k, uint64_t n, double* lo, double* hi);

/* Print a human-readable summary table to stdout. `label` e.g. "expert";
 * `wall_sec` total run wall time; `nthreads` resolved worker count. */
void metrics_print(const struct Metrics* m, const char* label, double wall_sec,
                   int nthreads);

#endif /* SOLVER_BENCH_METRICS_H */
