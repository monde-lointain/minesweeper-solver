/* policy_tuning.h — tunable constants for the info-gain guess policy.
 *
 * Kept in one header so tuning touches a single file. Orthodox C++: plain
 * constants. (HEUR_ names predate the deleted cheap heuristic; policy_infogain.cc
 * reuses them for its band + secondary progress key.)
 */
#ifndef SOLVER_BENCH_POLICY_TUNING_H
#define SOLVER_BENCH_POLICY_TUNING_H

/* Tolerance band on P(mine): cells within HEUR_BAND of the minimum risk are
 * treated as tied and ordered by info_gain then the progress score. 0 => ties
 * broken only on exact-equal risk (zero added risk). */
static const double HEUR_BAND = 0.02;

/* Progress-score weights (see policy_infogain.cc, the secondary key). Reward
 * making PROGRESS if the guess is safe — tightening constraints (frontier
 * connectivity) and opening new area (cascade). NOTE: an earlier "open-far /
 * interior" bias REGRESSED Expert (revealed isolated cells -> more guesses), so
 * it is intentionally absent.
 *   progress = W_CONNECT * (revealed-numbered-neighbor count / 8)
 *            + W_CASCADE * P(cell reveals 0)
 */
static const double HEUR_W_CONNECT = 1.0;
static const double HEUR_W_CASCADE = 0.5;

#endif /* SOLVER_BENCH_POLICY_TUNING_H */
