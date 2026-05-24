/* overlay.h — analysis overlay draw contract. FROZEN CONTRACT (Stream A.0).
 *
 * SDL/ImGui boundary: overlay_draw paints the probability tints, the best-move
 * ring, and the floating eval readout on top of the already-rendered board.
 * Callers gate the F10 on/off toggle; this draws when invoked.
 */
#ifndef SOLVER_OVERLAY_H
#define SOLVER_OVERLAY_H

#include "minesweeper/game.h"   /* struct Board */
#include "minesweeper/render.h" /* struct Layout */
#include "solver/engine.h"      /* struct Analysis */

/* Draw the analysis overlay for the current `a`/`b` using `lay` geometry.
 * No-op when the game is won/lost. Call between render_frame and ImGui::Render.
 */
void overlay_draw(const struct Analysis *a, const struct Board *b,
                  const struct Layout *lay);

#endif /* SOLVER_OVERLAY_H */
