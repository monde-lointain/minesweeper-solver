/* policy_infogain.cc — info-gain guess policy (paper's Inf(x)).
 *
 * Primary key unchanged from baseline: minimize P(mine). The win is the
 * tie-break among cells within HEUR_BAND of the minimum risk:
 *   1. max info_gain   — # cells forced (safe or mine) if this reveal is safe,
 *      the paper's Inf(x), computed exactly by the engine (re-deduction with
 * the cell pinned safe). This is the real version of the cheap connectivity
 *      proxy in policy_heuristic.
 *   2. max cascade     — P(cell reveals 0) ~= prod over covered neighbors of
 *      (1 - P(mine_n)), opening fresh area when no guess forces anything.
 *   3. row-major       — determinism.
 * Pure, no allocation. Opening (EVAL_START) pinned to the engine pick so it
 * stays identical to baseline (paired comparisons).
 */
#include "policy_infogain.h"

#include "policy_tuning.h"
#include "solver/engine.h"

/* P(cell (x,y) reveals 0) approximated as the product over covered neighbors of
 * (1 - P(mine)); revealed neighbors are known-safe (factor 1). */
static double cascade_score(const struct Board* b, const struct Analysis* a,
                            int x, int y) {
  double c = 1.0;
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
      if (!b->cells[idx].revealed) {
        c *= (1.0 - a->cells[idx].mine_prob);
      }
    }
  }
  return c;
}

int policy_infogain_select(const struct Board* b, const struct Analysis* a,
                           struct Move* out) {
  if (a->best_x < 0 || a->best_y < 0) {
    return -1; /* no covered cell (solved/terminal) */
  }
  /* Opening pinned to the engine's corner pick — identical to baseline. */
  if (a->eval == EVAL_START) {
    out->x = a->best_x;
    out->y = a->best_y;
    return 0;
  }

  /* Pass 1: minimum risk over covered, non-proven-mine cells. */
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

  /* Pass 2: among cells within the band of pmin, lexicographic
   * (info_gain, cascade), row-major first on ties. */
  double thresh = pmin + HEUR_BAND + 1e-12;
  int best_gain = -1;
  double best_cascade = -1.0;
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
      double cascade = cascade_score(b, a, x, y);
      if (gain > best_gain || (gain == best_gain && cascade > best_cascade)) {
        best_gain = gain;
        best_cascade = cascade;
        bx = x;
        by = y;
      }
    }
  }
  if (bx < 0) {
    return -1;
  }
  out->x = bx;
  out->y = by;
  return 0;
}
