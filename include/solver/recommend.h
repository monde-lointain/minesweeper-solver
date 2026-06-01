/* recommend.h — shared move recommendation (info-gain guess policy).
 *
 * solver_recommend_move returns the covered cell to play for an analysis: the
 * paper's min-risk + info-gain pick. The benchmark policy (policy_infogain.cc)
 * and the GUI overlay both call this, so the move the overlay highlights is the
 * exact move the benchmark plays — they agree by construction.
 *
 * Requires a->cells[].info_gain populated (solver_analyze_infogain) for the
 * info-gain tie-break to fire; without it the pick reduces to min-risk +
 * progress. Orthodox C++: POD, pointers, C headers.
 */
#ifndef SOLVER_RECOMMEND_H
#define SOLVER_RECOMMEND_H

#include "minesweeper/game.h" /* struct Board */
#include "solver/engine.h"    /* struct Analysis */

/* Recommended covered cell to reveal. Returns 0 and writes the chosen x,y
 * through out_x,out_y on success; returns -1 if no covered cell exists. Pure:
 * reads b and a, writes only the out pointers. */
int solver_recommend_move(const struct Board* b, const struct Analysis* a,
                          int* out_x, int* out_y);

/* Minimum P(mine) over covered, non-forced_mine cells; 2.0 if none exist. The
 * shared min-risk used both as solver_recommend_move's band anchor and as the
 * reasoning panel's "safest" readout, so the two can never disagree. Pure. */
double solver_min_risk(const struct Board* b, const struct Analysis* a);

#endif /* SOLVER_RECOMMEND_H */
