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
  double best_prob;     /* P(mine) of best move (0 when forced safe) */
  double interior_prob; /* uniform P(mine) for interior cells (eval line) */
  int interior_count;   /* number of interior cells (0 => none) */
  int eval;             /* enum SolverEval */
};

/* Per-analysis working memory (~2.5 MB), heap-allocated and opaque. The engine
 * is reentrant: give each thread/solver instance its own scratch; never share
 * one handle across threads. */
struct SolverScratch;
struct SolverScratch* solver_scratch_create(void);    /* calloc; NULL on OOM */
void solver_scratch_destroy(struct SolverScratch* s); /* free; NULL-safe */

/* Analyze the visible board state into `out`, using `s` as scratch. Pure
 * function of `b` (writes only `out` and `s`). `s` must be non-NULL. */
void solver_analyze(const struct Board* b, struct Analysis* out,
                    struct SolverScratch* s);

#endif /* SOLVER_ENGINE_H */
