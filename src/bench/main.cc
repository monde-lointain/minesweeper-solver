/* main.cc — benchmark CLI entry (Stream B.4).
 * Parse -> run -> print. Exit nonzero if a forced-safe death occurred (an engine
 * correctness defect the harness is designed to catch).
 */
#include <stdio.h>
#include <time.h>

#include "args.h"
#include "metrics.h"
#include "runner.h"

static double wall_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
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
    printf("running %s: %llux  seed=%u  threads=%s  policy=baseline ...\n",
           cfg.label, (unsigned long long)cfg.games, cfg.seed,
           (cfg.threads <= 0) ? "auto" : "fixed");
    fflush(stdout);
  }

  struct Metrics m;
  double t0 = wall_now();
  int nthreads = bench_run(&cfg, &m);
  double wall = wall_now() - t0;

  metrics_print(&m, cfg.label, wall, nthreads);
  return (m.deaths_on_forced_safe == 0) ? 0 : 1;
}
