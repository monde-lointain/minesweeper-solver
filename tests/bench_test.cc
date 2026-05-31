/* bench_test.cc — Stream C cross-module integration gate.
 *
 * Exercises the assembled harness (real policy + metrics + runner) end to end:
 * plausible winrate bands, the forced-safe-death correctness invariant on the
 * difficulties where the engine holds it, thread-invariance, and guess
 * calibration sanity.
 *
 * Winrate checks are plausibility BANDS, not exact golden counts: the engine
 * uses long double, so an exact count is not portable across compilers. The
 * strict determinism invariant (identical result across thread counts) lives in
 * bench_runner_test and is re-checked here.
 *
 * KNOWN DEFECT: on Expert the engine produces forced-safe deaths (P(mine)≈0 for
 * an actual mine) on dense fallback-path frontiers. Captured as a DISABLED
 * repro below; enable once the engine is fixed (out of Phase-1 scope).
 */
#include <gtest/gtest.h>
#include <string.h>

#include "runner.h"

static struct BenchConfig mkcfg(const char* label, int w, int h, int mines,
                                int threads, uint64_t games, uint32_t seed) {
  struct BenchConfig c;
  memset(&c, 0, sizeof c);
  c.width = w;
  c.height = h;
  c.mines = mines;
  c.games = games;
  c.seed = seed;
  c.threads = threads;
  c.policy_id = POLICY_BASELINE;
  c.quiet = true;
  strncpy(c.label, label, sizeof c.label - 1);
  return c;
}

TEST(BenchIntegration, BeginnerWinrateBandAndForcedSafe) {
  struct BenchConfig c = mkcfg("beginner", 9, 9, 10, 4, 2000, 1);
  struct Metrics m;
  bench_run(&c, &m);
  double wr = (double)m.wins / (double)m.games;
  EXPECT_GT(wr, 0.85);
  EXPECT_LT(wr, 0.93);
  EXPECT_EQ(m.deaths_on_forced_safe, 0u);
}

TEST(BenchIntegration, IntermediateWinrateBandAndForcedSafe) {
  struct BenchConfig c = mkcfg("intermediate", 16, 16, 40, 4, 2000, 1);
  struct Metrics m;
  bench_run(&c, &m);
  double wr = (double)m.wins / (double)m.games;
  EXPECT_GT(wr, 0.66);
  EXPECT_LT(wr, 0.77);
  EXPECT_EQ(m.deaths_on_forced_safe, 0u);
}

TEST(BenchIntegration, ThreadInvarianceRealModules) {
  struct BenchConfig c1 = mkcfg("intermediate", 16, 16, 40, 1, 1500, 99);
  struct BenchConfig c4 = mkcfg("intermediate", 16, 16, 40, 4, 1500, 99);
  struct Metrics m1;
  struct Metrics m4;
  bench_run(&c1, &m1);
  bench_run(&c4, &m4);
  EXPECT_EQ(m1.wins, m4.wins);
  EXPECT_EQ(m1.guess_moves, m4.guess_moves);
  EXPECT_EQ(m1.guesses_fatal, m4.guesses_fatal);
  EXPECT_EQ(m1.deaths_on_forced_safe, m4.deaths_on_forced_safe);
  for (int i = 0; i < METRICS_NBUCKET; ++i) {
    EXPECT_EQ(m1.cal_n[i], m4.cal_n[i]);
    EXPECT_EQ(m1.cal_mine[i], m4.cal_mine[i]);
  }
}

TEST(BenchIntegration, GuessCalibrationSane) {
  struct BenchConfig c = mkcfg("intermediate", 16, 16, 40, 4, 3000, 1);
  struct Metrics m;
  bench_run(&c, &m);
  ASSERT_GT(m.guess_moves, 0u);
  double mean_pred = (double)(m.guess_risk_sum / (long double)m.guess_moves);
  double fatal_rate = (double)m.guesses_fatal / (double)m.guess_moves;
  /* a well-calibrated engine prices its accepted guesses near their hit-rate */
  EXPECT_NEAR(mean_pred, fatal_rate, 0.05);
}

/* Regression: the engine must never assign forced-safe (P(mine)≈0 read as a
 * proof) to a cell that is actually a mine. Was nonzero on Expert before the
 * "approximate paths never masquerade as proofs" fix (engine write_cell_probs).
 * Seeds 1..5000 at seed base 1 previously produced 3 such deaths. */
TEST(BenchIntegration, ExpertNeverKillsOnForcedSafe) {
  struct BenchConfig c = mkcfg("expert", 30, 16, 99, 4, 5000, 1);
  struct Metrics m;
  bench_run(&c, &m);
  EXPECT_EQ(m.deaths_on_forced_safe, 0u);
}
