/* policy_heuristic.cc — progress-aware min-prob guess policy (Stream B.2).
 *
 * Primary key unchanged from baseline: minimize P(mine). The win is the
 * tie-break: among cells within HEUR_BAND of the minimum risk, prefer the one
 * that makes the most progress IF it turns out safe:
 *   - cascade likelihood  P(cell reveals 0) ~= prod over covered neighbors of
 *     (1 - P(mine_n))   (revealed neighbors are known-safe, factor 1);
 *   - opening potential   covered-neighbor count (proxy for cascade reach);
 *   - open-far bonus      distance to the nearest revealed cell (prefer an
 *     interior reveal into blank space over an equal-risk frontier reveal).
 * Validated by Liu et al. 2022 ("open far from solved areas"). Pure, no alloc.
 *
 * The opening move (EVAL_START) is pinned to the engine's pick (corner) so it
 * stays identical to the baseline — preserving paired comparisons.
 */
#include "policy_heuristic.h"

#include "policy_tuning.h"
#include "solver/engine.h"

/* Covered-neighbor count of (x,y); multiplies *cascade by (1 - P(mine)) for
 * each covered neighbor (cascade is P(this cell reveals a 0). */
static int neighbor_progress(const struct Board* b, const struct Analysis* a,
                             int x, int y, double* cascade) {
  double casc = 1.0;
  int cnt = 0;
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
        continue;
      }
      ++cnt;
      casc *= (1.0 - a->cells[idx].mine_prob);
    }
  }
  *cascade = casc;
  return cnt;
}

/* Chebyshev distance from (x,y) to the nearest revealed cell, capped at 8.
 * Larger == "further into blank space" == more information if it cascades. */
static int dist_to_revealed(const struct Board* b, int x, int y) {
  int best = 8;
  for (int ry = 0; ry < b->height; ++ry) {
    for (int rx = 0; rx < b->width; ++rx) {
      if (!b->cells[game_index(b, rx, ry)].revealed) {
        continue;
      }
      int ax = rx > x ? rx - x : x - rx;
      int ay = ry > y ? ry - y : y - ry;
      int d = ax > ay ? ax : ay;
      if (d < best) {
        best = d;
      }
    }
  }
  return best;
}

static double progress_score(const struct Board* b, const struct Analysis* a,
                             int x, int y) {
  double cascade = 1.0;
  int cnt = neighbor_progress(b, a, x, y, &cascade);
  double opening = (double)cnt / 8.0;
  double openfar = (double)dist_to_revealed(b, x, y) / 8.0;
  return HEUR_W_CASCADE * cascade + HEUR_W_OPENING * opening +
         HEUR_W_INTERIOR * openfar;
}

int policy_heuristic_select(const struct Board* b, const struct Analysis* a,
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

  /* Pass 2: among cells within the band of pmin, maximize progress (row-major
   * first on ties for determinism). */
  double thresh = pmin + HEUR_BAND + 1e-12;
  double best_score = -1.0;
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
      double score = progress_score(b, a, x, y);
      if (score > best_score) {
        best_score = score;
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
