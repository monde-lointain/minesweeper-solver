/* engine.h — pure Minesweeper probability engine (Stream A.0).
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
#include "solver/geom.h"       /* struct Pt */

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
  /* Info gain (paper's Inf(x)): # OTHER frontier cells that become provably
   * safe or provably mine if this cell is assumed safe. Pure constraint
   * propagation (never reads the hidden number). Populated by
   * solver_analyze_infogain only at EVAL_GUESS, for non-forced frontier cells
   * in an exact (non-fallback) component; 0 otherwise. */
  int info_gain;
};

/* Whole-board analysis. cells[] indexed via game_index. */
struct Analysis {
  struct CellAnalysis cells[BOARD_MAX_CELLS];
  struct Pt
      best; /* lowest-risk move over all covered cells; best.x < 0 if none */
  double best_prob;     /* P(mine) of best move (0 when forced safe) */
  double interior_prob; /* uniform P(mine) for interior cells (eval line) */
  int interior_count;   /* number of interior cells (0 => none) */
  int eval;             /* enum SolverEval */
  bool exact;           /* whole-board probabilities are proven exact (no
                         * component exceeded budget and fell back to the naive
                         * approximation). true for terminal/start/safe states.
                         * Populated by solver_analyze. */
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

/* As solver_analyze, then (only when the verdict is EVAL_GUESS) also fill
 * out->cells[].info_gain for every non-forced frontier cell in an exact
 * component — the paper's Inf(x): how many OTHER cells become provably safe or
 * mine if this cell is assumed safe (pure constraint propagation, no hidden
 * info). Separate entry point so the plain solver_analyze path pays nothing. */
void solver_analyze_infogain(const struct Board* b, struct Analysis* out,
                             struct SolverScratch* s);

#endif /* SOLVER_ENGINE_H */
