/* policy_infogain.h — info-gain guess policy (paper's Inf(x)).
 *
 * Thin adapter over solver_recommend_move (src/engine/recommend.cc), the shared
 * selector the GUI overlay also uses: min P(mine) primary, then max info_gain
 * (cells forced if this reveal is safe), then progress, then row-major.
 * Validated by Liu et al. 2022 (Knowledge-Based Systems 246).
 *
 * Requires the engine's info_gain field: the runner must analyze the board with
 * solver_analyze_infogain for this policy (see policy_needs_infogain).
 *
 * Orthodox C++: POD, pointers, C headers.
 */
#ifndef SOLVER_BENCH_POLICY_INFOGAIN_H
#define SOLVER_BENCH_POLICY_INFOGAIN_H

#include "minesweeper/game.h" /* struct Board */
#include "solver/engine.h"    /* struct Analysis */
#include "solver/geom.h"      /* struct Pt */

/* Choose a covered cell to reveal. Returns 0 and writes *out on success; returns
 * -1 if no covered cell exists. Pure: reads b and a, writes only *out. */
int policy_infogain_select(const struct Board* b, const struct Analysis* a,
                           struct Pt* out);

#endif /* SOLVER_BENCH_POLICY_INFOGAIN_H */
