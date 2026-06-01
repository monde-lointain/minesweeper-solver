/* geom.h — board-cell coordinate POD + the shared 8-neighbour traversal.
 *
 * Orthodox C++: a bare POD aggregate (initialize with {x, y}; no ctor/helper)
 * plus a free inline function. Used by the pure solver modules (engine,
 * recommend) so the 3x3 neighbour scan is written once.
 */
#ifndef SOLVER_GEOM_H
#define SOLVER_GEOM_H

#include "minesweeper/game.h" /* struct Board, game_index */

/* A board-cell coordinate. Bare POD; x < 0 conventionally means "none". */
struct Pt {
  int x;
  int y;
};

/* Neighbour k (0..7) of cell c: writes it to *out and returns true when in
 * bounds; returns false (out untouched) when off-board.
 *
 * The offset order is the canonical scan order (dy outer -1..1, dx inner
 * -1..1, centre skipped) that build_constraints relied on to assign variable
 * ids — and that order fixes the engine's floating-point accumulation order,
 * which golden_test pins by checksum. DO NOT REORDER these tables. */
static inline bool solver_neighbor(const struct Board* b, struct Pt c, int k,
                                   struct Pt* out) {
  static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
  int nx = c.x + dx[k];
  int ny = c.y + dy[k];
  if (nx < 0 || ny < 0 || nx >= b->width || ny >= b->height) {
    return false;
  }
  out->x = nx;
  out->y = ny;
  return true;
}

#endif /* SOLVER_GEOM_H */
