/* bench_metrics_test.cc — Stream B.2.
 * Wilson CI, calibration bucketing, decision/forced-safe accounting, and merge
 * commutativity.
 */
#include <gtest/gtest.h>

#include "metrics.h"
#include "solver/engine.h" /* EVAL_* */

TEST(BenchMetrics, Wilson95KnownValues) {
  double lo = -1.0;
  double hi = -1.0;
  metrics_wilson95(0, 0, &lo, &hi);
  EXPECT_EQ(lo, 0.0);
  EXPECT_EQ(hi, 0.0);

  metrics_wilson95(50, 100, &lo, &hi);
  EXPECT_NEAR(lo, 0.40383, 1e-4);
  EXPECT_NEAR(hi, 0.59617, 1e-4);

  metrics_wilson95(0, 100, &lo, &hi); /* lower bound ~0 (analytically 0) */
  EXPECT_NEAR(lo, 0.0, 1e-9);
  EXPECT_GT(hi, 0.0);
}

TEST(BenchMetrics, CalibrationBucketing) {
  struct Metrics m;
  metrics_zero(&m);
  metrics_record_decision(&m, EVAL_GUESS, 0.10, false, false); /* bucket 2 */
  metrics_record_decision(&m, EVAL_GUESS, 0.975, true, false); /* bucket 19 */
  metrics_record_decision(&m, EVAL_GUESS, 1.0, true, false);   /* clamp -> 19 */

  EXPECT_EQ(m.guess_moves, 3u);
  EXPECT_EQ(m.guesses_survived, 1u);
  EXPECT_EQ(m.guesses_fatal, 2u);
  EXPECT_EQ(m.cal_n[2], 1u);
  EXPECT_EQ(m.cal_mine[2], 0u);
  EXPECT_EQ(m.cal_n[19], 2u);
  EXPECT_EQ(m.cal_mine[19], 2u);
}

TEST(BenchMetrics, DecisionAccounting) {
  struct Metrics m;
  metrics_zero(&m);
  metrics_record_decision(&m, EVAL_START, 0.2, false, false);
  metrics_record_decision(&m, EVAL_SAFE, 0.0, false, true);
  metrics_record_decision(&m, EVAL_SAFE, 0.0, true, true); /* forced-safe death */

  EXPECT_EQ(m.start_moves, 1u);
  EXPECT_EQ(m.safe_moves, 2u);
  EXPECT_EQ(m.guess_moves, 0u);
  EXPECT_EQ(m.deaths_on_forced_safe, 1u);
}

TEST(BenchMetrics, GameAndAnalyzeRecording) {
  struct Metrics m;
  metrics_zero(&m);
  metrics_record_game(&m, true, 0.0);
  metrics_record_game(&m, false, 0.5);
  metrics_record_analyze(&m, 100);
  metrics_record_analyze(&m, 250);

  EXPECT_EQ(m.games, 2u);
  EXPECT_EQ(m.wins, 1u);
  EXPECT_NEAR((double)m.loss_progress_sum, 0.5, 1e-12);
  EXPECT_EQ(m.analyze_calls, 2u);
  EXPECT_EQ(m.analyze_ns_sum, 350u);
  EXPECT_EQ(m.analyze_ns_max, 250u);
}

TEST(BenchMetrics, MergeIsCommutative) {
  struct Metrics a;
  struct Metrics b;
  metrics_zero(&a);
  metrics_zero(&b);
  metrics_record_game(&a, true, 0.0);
  metrics_record_decision(&a, EVAL_GUESS, 0.30, false, false);
  metrics_record_analyze(&a, 500);
  metrics_record_game(&b, false, 0.7);
  metrics_record_decision(&b, EVAL_GUESS, 0.80, true, false);
  metrics_record_analyze(&b, 900);

  struct Metrics ab;
  struct Metrics ba;
  metrics_zero(&ab);
  metrics_zero(&ba);
  metrics_merge(&ab, &a);
  metrics_merge(&ab, &b);
  metrics_merge(&ba, &b);
  metrics_merge(&ba, &a);

  EXPECT_EQ(ab.games, ba.games);
  EXPECT_EQ(ab.wins, ba.wins);
  EXPECT_EQ(ab.guess_moves, ba.guess_moves);
  EXPECT_EQ(ab.guesses_fatal, ba.guesses_fatal);
  EXPECT_EQ(ab.cal_n[6], ba.cal_n[6]);  /* 0.30 -> bucket 6 */
  EXPECT_EQ(ab.cal_n[16], ba.cal_n[16]); /* 0.80 -> bucket 16 */
  EXPECT_EQ(ab.analyze_ns_sum, ba.analyze_ns_sum);
  EXPECT_EQ(ab.analyze_ns_max, ba.analyze_ns_max);
  EXPECT_EQ(ab.analyze_ns_max, 900u);
  EXPECT_NEAR((double)ab.loss_progress_sum, (double)ba.loss_progress_sum, 1e-12);
}
