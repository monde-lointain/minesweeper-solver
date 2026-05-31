/* metrics.cc — benchmark metrics accumulation + reporting (Stream B.2).
 *
 * All fields are commutative reductions (sum / max), so per-worker Metrics
 * merge to the same aggregate independent of thread count or order.
 */
#include "metrics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "solver/engine.h" /* enum SolverEval */

void metrics_zero(struct Metrics* m) { memset(m, 0, sizeof *m); }

void metrics_merge(struct Metrics* dst, const struct Metrics* src) {
  dst->games += src->games;
  dst->wins += src->wins;
  dst->start_moves += src->start_moves;
  dst->safe_moves += src->safe_moves;
  dst->guess_moves += src->guess_moves;
  dst->guesses_survived += src->guesses_survived;
  dst->guesses_fatal += src->guesses_fatal;
  dst->guess_risk_sum += src->guess_risk_sum;
  for (int i = 0; i < METRICS_NBUCKET; ++i) {
    dst->cal_n[i] += src->cal_n[i];
    dst->cal_pred_sum[i] += src->cal_pred_sum[i];
    dst->cal_mine[i] += src->cal_mine[i];
  }
  dst->deaths_on_forced_safe += src->deaths_on_forced_safe;
  dst->loss_progress_sum += src->loss_progress_sum;
  dst->analyze_calls += src->analyze_calls;
  dst->analyze_ns_sum += src->analyze_ns_sum;
  if (src->analyze_ns_max > dst->analyze_ns_max) {
    dst->analyze_ns_max = src->analyze_ns_max;
  }
}

void metrics_record_decision(struct Metrics* m, int eval, double pred,
                             bool was_mine, bool forced_safe) {
  if (eval == EVAL_START) {
    m->start_moves += 1;
  } else if (eval == EVAL_SAFE) {
    m->safe_moves += 1;
  } else if (eval == EVAL_GUESS) {
    m->guess_moves += 1;
    m->guess_risk_sum += (long double)pred;
    if (was_mine) {
      m->guesses_fatal += 1;
    } else {
      m->guesses_survived += 1;
    }
    int bi = (int)(pred * (double)METRICS_NBUCKET);
    if (bi < 0) {
      bi = 0;
    }
    if (bi >= METRICS_NBUCKET) {
      bi = METRICS_NBUCKET - 1;
    }
    m->cal_n[bi] += 1;
    m->cal_pred_sum[bi] += (long double)pred;
    if (was_mine) {
      m->cal_mine[bi] += 1;
    }
  }
  if (was_mine && forced_safe) {
    m->deaths_on_forced_safe += 1;
  }
}

void metrics_record_game(struct Metrics* m, bool won, double loss_progress) {
  m->games += 1;
  if (won) {
    m->wins += 1;
  } else {
    m->loss_progress_sum += (long double)loss_progress;
  }
}

void metrics_record_analyze(struct Metrics* m, uint64_t ns) {
  m->analyze_calls += 1;
  m->analyze_ns_sum += ns;
  if (ns > m->analyze_ns_max) {
    m->analyze_ns_max = ns;
  }
}

void metrics_wilson95(uint64_t k, uint64_t n, double* lo, double* hi) {
  if (n == 0) {
    *lo = 0.0;
    *hi = 0.0;
    return;
  }
  double z = 1.959963984540054; /* 97.5th percentile of N(0,1) */
  double nn = (double)n;
  double phat = (double)k / nn;
  double z2 = z * z;
  double denom = 1.0 + z2 / nn;
  double centre = phat + z2 / (2.0 * nn);
  double margin = z * sqrt((phat * (1.0 - phat) + z2 / (4.0 * nn)) / nn);
  double lo_v = (centre - margin) / denom;
  double hi_v = (centre + margin) / denom;
  *lo = (lo_v < 0.0) ? 0.0 : lo_v;
  *hi = (hi_v > 1.0) ? 1.0 : hi_v;
}

void metrics_print(const struct Metrics* m, const char* label, double wall_sec,
                   int nthreads) {
  uint64_t games = m->games;
  uint64_t wins = m->wins;
  uint64_t losses = games - wins;
  double winrate = (games != 0) ? (double)wins / (double)games : 0.0;
  double lo = 0.0;
  double hi = 0.0;
  metrics_wilson95(wins, games, &lo, &hi);

  uint64_t deduced = m->safe_moves + m->guess_moves; /* excludes opening */
  double guess_frac =
      (deduced != 0) ? (double)m->guess_moves / (double)deduced : 0.0;
  double mean_risk =
      (m->guess_moves != 0)
          ? (double)(m->guess_risk_sum / (long double)m->guess_moves)
          : 0.0;
  double guess_surv = (m->guess_moves != 0)
                          ? (double)m->guesses_survived / (double)m->guess_moves
                          : 0.0;
  double mean_loss_depth =
      (losses != 0) ? (double)(m->loss_progress_sum / (long double)losses)
                    : 0.0;
  double mean_us =
      (m->analyze_calls != 0)
          ? (double)m->analyze_ns_sum / (double)m->analyze_calls / 1000.0
          : 0.0;
  double max_us = (double)m->analyze_ns_max / 1000.0;
  double gps = (wall_sec > 0.0) ? (double)games / wall_sec : 0.0;

  printf("\n==== minesweeper-bench: %s ====\n", label);
  printf("games            %llu  (threads=%d, %.2fs, %.0f games/s)\n",
         (unsigned long long)games, nthreads, wall_sec, gps);
  printf("winrate          %.4f  [%.4f, %.4f]  95%% Wilson\n", winrate, lo, hi);
  printf("decisions        opening=%llu safe=%llu guess=%llu\n",
         (unsigned long long)m->start_moves, (unsigned long long)m->safe_moves,
         (unsigned long long)m->guess_moves);
  printf("forced-guess     %.4f of deduced moves\n", guess_frac);
  printf("guess risk(mean) %.4f   guess survival %.4f\n", mean_risk,
         guess_surv);
  printf("deaths@forced-safe %llu  (MUST be 0)\n",
         (unsigned long long)m->deaths_on_forced_safe);
  printf("mean loss depth  %.4f revealed-fraction\n", mean_loss_depth);
  printf("analyze          %.1f us mean, %.1f us max (%llu calls)\n", mean_us,
         max_us, (unsigned long long)m->analyze_calls);

  printf("calibration (chosen guesses)\n");
  printf("   bucket          n       pred     empirical\n");
  for (int i = 0; i < METRICS_NBUCKET; ++i) {
    if (m->cal_n[i] == 0) {
      continue;
    }
    double blo = (double)i / (double)METRICS_NBUCKET;
    double bhi = (double)(i + 1) / (double)METRICS_NBUCKET;
    double pred = (double)(m->cal_pred_sum[i] / (long double)m->cal_n[i]);
    double emp = (double)m->cal_mine[i] / (double)m->cal_n[i];
    printf("   [%.2f,%.2f)  %10llu     %.3f      %.3f\n", blo, bhi,
           (unsigned long long)m->cal_n[i], pred, emp);
  }
}
