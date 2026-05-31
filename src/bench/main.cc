/* main.cc — STUB (Stream A). Finalized by Stream B.4.
 * Honors --help; otherwise runs a tiny fixed smoke so a bare invocation is fast
 * (the real main, B.4, honors --games / the 1M default).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "args.h"
#include "metrics.h"
#include "runner.h"

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      bench_usage(argv[0]);
      return 0;
    }
  }

  struct BenchConfig cfg;
  const char* err = NULL;
  bench_parse_args(argc, argv, &cfg, &err);
  cfg.games = 5; /* stub: keep a bare run fast */

  struct Metrics m;
  int nthreads = bench_run(&cfg, &m);
  metrics_print(&m, cfg.label, 0.0, nthreads);
  return 0;
}
