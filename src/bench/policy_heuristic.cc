/* policy_heuristic.cc — progress-aware min-prob guess policy (Stream B.2).
 *
 * STUB (Stream A): forwards the engine's baseline pick so the seam builds
 * green; Stream B.2 replaces the body with the real progress-aware tie-break.
 */
#include "policy_heuristic.h"

int policy_heuristic_select(const struct Board* b, const struct Analysis* a,
                            struct Move* out) {
  (void)b;
  if (a->best_x < 0 || a->best_y < 0) {
    return -1;
  }
  out->x = a->best_x;
  out->y = a->best_y;
  return 0;
}
