/* policy_infogain.cc — info-gain guess policy (paper's Inf(x)).
 *
 * Thin adapter over solver_recommend_move (src/engine/recommend.cc) — the
 * shared selector the GUI overlay also calls, so the benchmark and the overlay
 * play the identical move. The selection logic (min-risk primary, info_gain
 * then progress tie-break, opening pinned) lives in recommend.cc.
 */
#include "policy_infogain.h"

#include "solver/recommend.h"

int policy_infogain_select(const struct Board* b, const struct Analysis* a,
                           struct Move* out) {
  struct Pt p;
  int rc = solver_recommend_move(b, a, &p);
  if (rc == 0) {
    out->x = p.x;
    out->y = p.y;
  }
  return rc;
}
