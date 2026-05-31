/* args.h — CLI parsing for the benchmark harness.
 *
 * Orthodox C++: pointers, C headers, return codes (no exceptions).
 */
#ifndef SOLVER_BENCH_ARGS_H
#define SOLVER_BENCH_ARGS_H

#include "runner.h"

/* Parse argv into *cfg (pre-filled with defaults by the parser).
 * Returns:
 *   0  -> run with *cfg
 *   1  -> help requested; caller prints usage and exits 0 (*errmsg unset)
 *  -1  -> parse error; *errmsg = static message, caller prints + exits nonzero
 */
int bench_parse_args(int argc, char** argv, struct BenchConfig* cfg,
                     const char** errmsg);

/* Print usage text to stdout. */
void bench_usage(const char* prog);

#endif /* SOLVER_BENCH_ARGS_H */
