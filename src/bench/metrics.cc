/* metrics.cc — STUB (Stream A). Replaced by Stream B.2.
 * Minimal tally (games/wins/analyze) so the harness builds and runs; no
 * calibration, no real Wilson interval, no full summary.
 */
#include "metrics.h"

#include <stdio.h>
#include <string.h>

void metrics_zero(struct Metrics* m) { memset(m, 0, sizeof *m); }

void metrics_merge(struct Metrics* dst, const struct Metrics* src) {
  dst->games += src->games;
  dst->wins += src->wins;
  dst->analyze_calls += src->analyze_calls;
}

void metrics_record_decision(struct Metrics* m, int eval, double pred,
                             bool was_mine, bool forced_safe) {
  (void)m;
  (void)eval;
  (void)pred;
  (void)was_mine;
  (void)forced_safe;
}

void metrics_record_game(struct Metrics* m, bool won, double loss_progress) {
  (void)loss_progress;
  m->games += 1;
  if (won) {
    m->wins += 1;
  }
}

void metrics_record_analyze(struct Metrics* m, uint64_t ns) {
  (void)ns;
  m->analyze_calls += 1;
}

void metrics_wilson95(uint64_t k, uint64_t n, double* lo, double* hi) {
  double p = (n != 0) ? (double)k / (double)n : 0.0;
  *lo = p;
  *hi = p;
}

void metrics_print(const struct Metrics* m, const char* label, double wall_sec,
                   int nthreads) {
  (void)nthreads;
  double rate = (m->games != 0) ? (double)m->wins / (double)m->games : 0.0;
  printf("[%s] games=%llu wins=%llu winrate=%.4f (%.2fs)\n", label,
         (unsigned long long)m->games, (unsigned long long)m->wins, rate,
         wall_sec);
}
