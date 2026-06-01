/* main.cc — benchmark CLI entry (Stream B.4).
 * Parse -> run -> print. Exit nonzero if a forced-safe death occurred (an
 * engine correctness defect the harness is designed to catch).
 */
#include <stdio.h>

#include <chrono>

#include "args.h"
#include "metrics.h"
#include "runner.h"

/* Monotonic elapsed-time source in seconds. std::chrono::steady_clock is a
 * sanctioned Modern-C++ carve-out: the only portable monotonic source
 * (clock_gettime is POSIX-only, QueryPerformanceCounter is Win32-only). */
static double wall_now(void) {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int main(int argc, char** argv) {
  struct BenchConfig cfg;
  const char* err = NULL;
  int rc = bench_parse_args(argc, argv, &cfg, &err);
  if (rc == 1) {
    bench_usage(argv[0]);
    return 0;
  }
  if (rc < 0) {
    fprintf(stderr, "error: %s\n", (err != NULL) ? err : "bad arguments");
    bench_usage(argv[0]);
    return 2;
  }

  if (!cfg.quiet) {
    printf("running %s: %llux  seed=%u  threads=%s  policy=%s ...\n", cfg.label,
           (unsigned long long)cfg.games, cfg.seed,
           (cfg.threads <= 0) ? "auto" : "fixed",
           (cfg.policy_id == POLICY_BASELINE) ? "baseline" : "infogain");
    fflush(stdout);
  }

  struct Metrics m;
  double t0 = wall_now();
  int nthreads = bench_run(&cfg, &m);
  double wall = wall_now() - t0;

  metrics_print(&m, cfg.label, wall, nthreads);
  return (m.deaths_on_forced_safe == 0) ? 0 : 1;
}
