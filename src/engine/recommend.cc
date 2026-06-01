/* recommend.cc — shared "best move to play" selector (info-gain guess policy).
 *
 * Single source of truth for the move recommendation. The bench win-rate policy
 * (policy_infogain.cc) and the GUI overlay both call solver_recommend_move, so
 * the cell the overlay highlights is exactly the cell the benchmark plays.
 *
 * Primary key: minimize P(mine). Among cells within RECOMMEND_BAND of the
 * minimum risk, prefer max info_gain (paper's Inf(x) — # cells forced if this
 * reveal is safe; filled by solver_analyze_infogain), then max progress
 * (frontier connectivity + cascade likelihood), then row-major for determinism.
 * Opening (EVAL_START) is pinned to the engine pick. The band is suppressed at
 * EVAL_SAFE (a proven-safe cell exists) so certain safety is never traded for
 * progress. Pure, no allocation.
 *
 * Orthodox C++: POD, pointers, C headers.
 */
#include "solver/recommend.h"

#include "solver/engine.h"

/* Tolerance band on P(mine): cells within this of the minimum risk are treated
 * as tied and ordered by info_gain then progress. 0 => ties broken only on
 * exact-equal risk. (Was the bench's HEUR_BAND.) */
static const double RECOMMEND_BAND = 0.02;

/* Progress weights: reward making PROGRESS if the guess is safe — tightening
 * constraints (frontier connectivity) and opening new area (cascade). An
 * "open-far / interior" bias REGRESSED Expert, so it is intentionally absent.
 *   progress = W_CONNECT * (revealed-numbered-neighbor count / 8)
 *            + W_CASCADE * P(cell reveals 0) */
static const double RECOMMEND_W_CONNECT = 1.0;
static const double RECOMMEND_W_CASCADE = 0.5;

/* Progress proxy if (x,y) turns out safe: frontier connectivity (count of
 * revealed-numbered neighbors) + cascade likelihood (product of neighbor
 * safety). Secondary key, so when no band candidate forces anything (info_gain
 * all 0 — the common case) the pick is the proven heuristic and info_gain only
 * adds a preference for forcing guesses on top. */
static double progress_score(const struct Board* b, const struct Analysis* a,
                             int x, int y) {
  double cascade = 1.0;
  int connect = 0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      int nx = x + dx;
      int ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= b->width || ny >= b->height) {
        continue;
      }
      int idx = game_index(b, nx, ny);
      if (b->cells[idx].revealed) {
        ++connect;
      } else {
        cascade *= (1.0 - a->cells[idx].mine_prob);
      }
    }
  }
  return RECOMMEND_W_CONNECT * ((double)connect / 8.0) +
         RECOMMEND_W_CASCADE * cascade;
}

double solver_min_risk(const struct Board* b, const struct Analysis* a) {
  double pmin = 2.0;
  for (int i = 0; i < b->width * b->height; ++i) {
    if (b->cells[i].revealed || a->cells[i].forced_mine) {
      continue;
    }
    double p = a->cells[i].mine_prob;
    if (p < pmin) {
      pmin = p;
    }
  }
  return pmin;
}

int solver_recommend_move(const struct Board* b, const struct Analysis* a,
                          int* out_x, int* out_y) {
  if (a->best_x < 0 || a->best_y < 0) {
    return -1; /* no covered cell (solved/terminal) */
  }
  /* Opening pinned to the engine's corner pick — identical to baseline. */
  if (a->eval == EVAL_START) {
    *out_x = a->best_x;
    *out_y = a->best_y;
    return 0;
  }

  /* Pass 1: minimum risk over covered, non-proven-mine cells. */
  double pmin = solver_min_risk(b, a);

  /* Pass 2: among cells within the band of pmin, lexicographic
   * (info_gain, progress), row-major first on ties. The band is suppressed at
   * EVAL_SAFE so a proven-safe (0%) cell is never passed over for a (0,band]
   * cell — trading certain safety for progress there is strictly dominated
   * (the informative cell stays available; a paired 200k-Expert test favored
   * the gate, McNemar chi2=17.9). */
  double band = (a->eval == EVAL_SAFE) ? 0.0 : RECOMMEND_BAND;
  double thresh = pmin + band + 1e-12;
  int best_gain = -1;
  double best_prog = -1.0;
  int bx = -1;
  int by = -1;
  for (int y = 0; y < b->height; ++y) {
    for (int x = 0; x < b->width; ++x) {
      int i = game_index(b, x, y);
      if (b->cells[i].revealed || a->cells[i].forced_mine) {
        continue;
      }
      if (a->cells[i].mine_prob > thresh) {
        continue;
      }
      int gain = a->cells[i].info_gain;
      double prog = progress_score(b, a, x, y);
      if (gain > best_gain || (gain == best_gain && prog > best_prog)) {
        best_gain = gain;
        best_prog = prog;
        bx = x;
        by = y;
      }
    }
  }
  if (bx < 0) {
    return -1;
  }
  *out_x = bx;
  *out_y = by;
  return 0;
}
