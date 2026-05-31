/* policy_tuning.h — tunable constants for the guess policies (Stream A→C).
 *
 * Defaults here; Stream C locks final values from harness sweeps. Kept in one
 * header so tuning touches a single file. Orthodox C++: plain constants.
 */
#ifndef SOLVER_BENCH_POLICY_TUNING_H
#define SOLVER_BENCH_POLICY_TUNING_H

/* Tolerance band on P(mine): cells within HEUR_BAND of the minimum risk are
 * treated as tied and ordered by the progress score. 0 ⇒ progress breaks only
 * exact ties (zero added risk). */
static const double HEUR_BAND = 0.0;

/* Progress-score weights (see policy_heuristic.cc):
 *   w1 · P(cell reveals 0 → cascade)
 * + w2 · (covered-neighbor count, normalized)
 * + w3 · (interior / open-far-from-solved bonus)
 */
static const double HEUR_W_CASCADE = 1.0;
static const double HEUR_W_OPENING = 0.25;
static const double HEUR_W_INTERIOR = 0.5;

#endif /* SOLVER_BENCH_POLICY_TUNING_H */
