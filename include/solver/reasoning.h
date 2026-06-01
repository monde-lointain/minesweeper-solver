/* reasoning.h — pure extraction of a display-ready "train of thought" view from
 * a cached Analysis. NO SDL/ImGui, so it is unit-tested without a window.
 * Orthodox C++: POD, pointers, C headers. */
#ifndef SOLVER_REASONING_H
#define SOLVER_REASONING_H

#include <stdbool.h>

#include "minesweeper/game.h" /* struct Board, game_index */
#include "solver/engine.h"    /* struct Analysis, enum SolverEval */
#include "solver/geom.h"      /* struct Pt */

/* Display-ready snapshot. All percentages are 0..100 ints. */
struct ReasoningView {
  int verdict; /* enum SolverEval */
  bool exact;  /* Analysis.exact */

  bool has_move; /* a covered cell is recommended */
  struct Pt move;
  int risk_pct;      /* P(mine) % of the recommended cell */
  int safest_pct;    /* min P(mine) % over covered, non-proven-mine cells */
  bool took_riskier; /* pick risk > safest (band tie-break overrode min-risk) */
  int pick_gain;     /* info_gain of the recommended cell */

  int proven_safe;    /* # covered forced_safe cells */
  int proven_mine;    /* # covered forced_mine cells */
  int frontier;       /* # covered is_frontier cells */
  int interior_pct;   /* interior_prob % */
  int interior_count; /* interior cell count */
  int mines_total;    /* board total mines */

  /* hover inspect (a covered cell under the game-window cursor) */
  bool hover_valid;
  struct Pt hover;
  int hover_pct;
  bool hover_forced_safe;
  bool hover_forced_mine;
  bool hover_frontier;
  int hover_gain;
};

/* Fill `out` from the board + cached analysis. `hover` is the covered cell
 * under the game cursor, or {<0,<0} if none. The recommended move equals
 * solver_recommend_move (same cell the overlay box highlights). Pure: reads b
 * and a, writes only out. */
void reasoning_build(const struct Board* b, const struct Analysis* a,
                     struct Pt hover, struct ReasoningView* out);

#endif /* SOLVER_REASONING_H */
