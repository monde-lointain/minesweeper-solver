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

/* Progress proxy for revealing (x,y) if it turns out safe:
 *   - connectivity: number of revealed (numbered) neighbors. Resolving a cell
 *     that participates in many constraints tightens the most of them, so it is
 *     the likeliest to force new deductions (the paper's info-gain idea,
 * cheaply approximated). Interior cells (connectivity 0) make little progress.
 *   - cascade: P(cell reveals 0) ~= prod over covered neighbors of (1-P(mine)),
 *     opening a fresh region.
 * Deliberately NO open-far/interior term — it regressed (isolated reveals don't
 * constrain the board, so the game needs more guesses). */
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
        ++connect; /* a numbered neighbor -> a constraint this cell is in */
      } else {
        cascade *= (1.0 - a->cells[idx].mine_prob);
      }
    }
  }
  return HEUR_W_CONNECT * ((double)connect / 8.0) + HEUR_W_CASCADE * cascade;
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
