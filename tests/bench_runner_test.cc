/* bench_runner_test.cc — Stream B.3.
 * Determinism (same seed -> same aggregate), thread-invariance (1 vs 4
 * threads), exact game count, and the forced-safe-death correctness invariant.
 */
#include <gtest/gtest.h>
#include <string.h>

#include "runner.h"

static struct BenchConfig mkcfg(int threads, uint64_t games) {
  struct BenchConfig c;
  memset(&c, 0, sizeof c);
  c.width = 9;
  c.height = 9;
  c.mines = 10;
  c.games = games;
  c.seed = 42u;
  c.threads = threads;
  c.policy_id = POLICY_BASELINE;
  c.quiet = true;
  strcpy(c.label, "beginner");
  return c;
}

/* Compare every accumulator except the timing fields (wall-dependent). */
static void expect_same_aggregate(const struct Metrics* a,
                                  const struct Metrics* b) {
  EXPECT_EQ(a->games, b->games);
  EXPECT_EQ(a->wins, b->wins);
  EXPECT_EQ(a->start_moves, b->start_moves);
  EXPECT_EQ(a->safe_moves, b->safe_moves);
  EXPECT_EQ(a->guess_moves, b->guess_moves);
  EXPECT_EQ(a->guesses_survived, b->guesses_survived);
  EXPECT_EQ(a->guesses_fatal, b->guesses_fatal);
  EXPECT_EQ(a->deaths_on_forced_safe, b->deaths_on_forced_safe);
  for (int i = 0; i < METRICS_NBUCKET; ++i) {
    EXPECT_EQ(a->cal_n[i], b->cal_n[i]);
    EXPECT_EQ(a->cal_mine[i], b->cal_mine[i]);
  }
  EXPECT_NEAR((double)a->loss_progress_sum, (double)b->loss_progress_sum, 1e-9);
  EXPECT_NEAR((double)a->guess_risk_sum, (double)b->guess_risk_sum, 1e-9);
}

TEST(BenchRunner, DeterministicAndThreadInvariant) {
  struct BenchConfig c1 = mkcfg(1, 300);
  struct Metrics m1;
  int t1 = bench_run(&c1, &m1);
  EXPECT_EQ(t1, 1);

  struct BenchConfig c1b = mkcfg(1, 300);
  struct Metrics m1b;
  bench_run(&c1b, &m1b);
  expect_same_aggregate(&m1, &m1b); /* reproducible */

  struct BenchConfig c4 = mkcfg(4, 300);
  struct Metrics m4;
  int t4 = bench_run(&c4, &m4);
  EXPECT_EQ(t4, 4);
  expect_same_aggregate(&m1, &m4); /* identical regardless of thread count */

  EXPECT_EQ(m1.games, 300u);
  EXPECT_EQ(m1.deaths_on_forced_safe, 0u); /* engine never mislabels a safe */
  EXPECT_GT(m1.wins, 0u);                  /* beginner: many wins expected */
}

TEST(BenchRunner, AutoThreadsResolveAndZeroGames) {
  struct BenchConfig c = mkcfg(0, 0); /* auto threads, no games */
  struct Metrics m;
  int t = bench_run(&c, &m);
  EXPECT_GE(t, 1);
  EXPECT_EQ(m.games, 0u);
}
