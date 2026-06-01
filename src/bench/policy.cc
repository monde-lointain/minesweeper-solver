/* policy.cc — move-selection policies (Stream B.1).
 *
 * POLICY_BASELINE reproduces the engine's own pick: solver_analyze already
 * selects the lowest-mine-probability covered cell (row-major tie-break) into
 * Analysis.best_x/best_y, so the baseline simply forwards it. This makes the
 * Phase-1 benchmark measure the current solver's true winrate.
 */
#include "policy.h"

#include "policy_heuristic.h"
#include "policy_infogain.h"

int policy_needs_infogain(int policy_id) {
  return policy_id == POLICY_INFOGAIN ? 1 : 0;
}

int policy_select(int policy_id, const struct Board* b,
                  const struct Analysis* a, struct Move* out) {
  if (policy_id == POLICY_HEURISTIC) {
    return policy_heuristic_select(b, a, out);
  }
  if (policy_id == POLICY_INFOGAIN) {
    return policy_infogain_select(b, a, out);
  }
  /* POLICY_BASELINE: forward the engine's precomputed min-prob pick. */
  (void)b;
  if (a->best_x < 0 || a->best_y < 0) {
    return -1; /* no covered cell (solved/terminal) */
  }
  out->x = a->best_x;
  out->y = a->best_y;
  return 0;
}
