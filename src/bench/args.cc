/* args.cc — STUB (Stream A). Replaced by Stream B.4.
 * Fills Expert defaults and ignores argv (no real parsing yet).
 */
#include "args.h"

#include <stdio.h>
#include <string.h>

int bench_parse_args(int argc, char** argv, struct BenchConfig* cfg,
                     const char** errmsg) {
  (void)argc;
  (void)argv;
  cfg->width = 30;
  cfg->height = 16;
  cfg->mines = 99;
  cfg->games = 1000000;
  cfg->seed = 1;
  cfg->threads = 0;
  cfg->policy_id = POLICY_BASELINE;
  cfg->quiet = false;
  strcpy(cfg->label, "expert");
  *errmsg = NULL;
  return 0;
}

void bench_usage(const char* prog) {
  printf("usage: %s [--difficulty beginner|intermediate|expert] [--games N]\n",
         prog);
}
