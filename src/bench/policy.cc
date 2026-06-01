/* policy.cc — move-selection policies.
 *
 * POLICY_INFOGAIN (default) is the paper's Inf(x) guess policy. POLICY_BASELINE
 * reproduces the engine's own pick: solver_analyze already selects the
 * lowest-mine-probability covered cell (row-major tie-break) into
 * Analysis.best_x/best_y, so the baseline simply forwards it — the reference
 * for measuring engine accuracy without policy confound.
 */
#include "policy.h"

#include "policy_infogain.h"

int policy_needs_infogain(int policy_id) {
  return policy_id == POLICY_INFOGAIN ? 1 : 0;
}

int policy_select(int policy_id, const struct Board* b,
                  const struct Analysis* a, struct Move* out) {
  if (policy_id == POLICY_INFOGAIN) {
    return policy_infogain_select(b, a, out);
  }
  /* POLICY_BASELINE: forward the engine's precomputed min-prob pick. */
  (void)b;
  if (a->best.x < 0 || a->best.y < 0) {
    return -1; /* no covered cell (solved/terminal) */
  }
  out->x = a->best.x;
  out->y = a->best.y;
  return 0;
}
