/* policy_heuristic.h — progress-aware min-prob guess policy (Stream B.2).
 *
 * Primary key is unchanged from baseline (minimize P(mine)); the win is the
 * tie-break: among cells within a tolerance band of the minimum risk, prefer the
 * one that makes the most progress if it turns out safe (cascade likelihood,
 * interior / "open-far-from-solved" preference). Validated by Liu et al. 2022
 * (Knowledge-Based Systems 246). Also intended as a cheap, allocation-free unit.
 *
 * Orthodox C++: POD, pointers, C headers.
 */
#ifndef SOLVER_BENCH_POLICY_HEURISTIC_H
#define SOLVER_BENCH_POLICY_HEURISTIC_H

#include "minesweeper/game.h" /* struct Board */
#include "policy.h"           /* struct Move */
#include "solver/engine.h"    /* struct Analysis */

/* Choose a covered cell to reveal. Returns 0 and writes *out on success; returns
 * -1 if no covered cell exists. Pure: reads b and a, writes only *out. */
int policy_heuristic_select(const struct Board* b, const struct Analysis* a,
                            struct Move* out);

#endif /* SOLVER_BENCH_POLICY_HEURISTIC_H */
