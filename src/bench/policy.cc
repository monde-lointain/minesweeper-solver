/* policy.cc — STUB (Stream A). Replaced by Stream B.1.
 * First covered cell, row-major. Lets games terminate so the harness builds and
 * runs end-to-end before the real baseline policy lands.
 */
#include "policy.h"

int policy_select(int policy_id, const struct Board* b,
                  const struct Analysis* a, struct Move* out) {
  (void)policy_id;
  (void)a;
  for (int y = 0; y < b->height; ++y) {
    for (int x = 0; x < b->width; ++x) {
      if (!b->cells[game_index(b, x, y)].revealed) {
        out->x = x;
        out->y = y;
        return 0;
      }
    }
  }
  return -1;
}
