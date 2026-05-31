/* policy.cc — move-selection policies (Stream B.1).
 *
 * POLICY_BASELINE reproduces the engine's own pick: solver_analyze already
 * selects the lowest-mine-probability covered cell (row-major tie-break) into
 * Analysis.best_x/best_y, so the baseline simply forwards it. This makes the
 * Phase-1 benchmark measure the current solver's true winrate.
 */
#include "policy.h"

int policy_select(int policy_id, const struct Board* b,
                  const struct Analysis* a, struct Move* out) {
  (void)policy_id; /* only POLICY_BASELINE today */
  (void)b;         /* baseline reads the engine's precomputed best move */
  if (a->best_x < 0 || a->best_y < 0) {
    return -1; /* no covered cell (solved/terminal) */
  }
  out->x = a->best_x;
  out->y = a->best_y;
  return 0;
}
