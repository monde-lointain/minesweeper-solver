/* engine.h — pure Minesweeper probability engine. FROZEN CONTRACT (Stream A.0).
 *
 * NO SDL/ImGui. Reads only what the player sees (revealed `adjacent` counts,
 * geometry, total `mines`); never reads `cell.mine` for a covered cell and
 * never trusts `flag` state. Orthodox C++: POD, plain enums, pointers, C
 * headers.
 */
#ifndef SOLVER_ENGINE_H
#define SOLVER_ENGINE_H

#include "minesweeper/game.h"  /* struct Board/Cell, game_index */
#include "minesweeper/types.h" /* BOARD_MAX_CELLS */

/* Position evaluation, analogous to a chess engine's verdict. */
enum SolverEval {
  EVAL_START = 0,  /* no reveals yet — opening heuristic */
  EVAL_SAFE = 1,   /* a forced-safe (0% mine) move exists */
  EVAL_GUESS = 2,  /* no safe cell; best move is a probabilistic guess */
  EVAL_SOLVED = 3, /* game won */
  EVAL_LOST = 4    /* game lost */
};

/* Per-cell verdict. mine_prob is meaningful for covered cells only. */
struct CellAnalysis {
  double mine_prob; /* P(mine) in [0,1] */
  bool is_frontier; /* covered, adjacent to a revealed number (flags ignored) */
  bool forced_safe; /* proven safe (prob 0 by deduction) */
  bool forced_mine; /* proven mine */
};

/* Whole-board analysis. cells[] indexed via game_index. */
struct Analysis {
  struct CellAnalysis cells[BOARD_MAX_CELLS];
  int best_x; /* lowest-risk move over all covered cells; -1 if none */
  int best_y;
  double best_prob;      /* P(mine) of best move (0 when forced safe) */
  bool best_is_interior; /* true if best move is an interior (un-tinted) cell */
  double interior_prob;  /* uniform P(mine) for interior cells (eval line) */
  int interior_count;    /* number of interior cells (0 => none) */
  int eval;              /* enum SolverEval */
};

/* Analyze the visible board state into `out`. Pure function of `b`. */
void solver_analyze(const struct Board *b, struct Analysis *out);

#endif /* SOLVER_ENGINE_H */
