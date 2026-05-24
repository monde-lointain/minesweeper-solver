/* engine.cc — probability engine. STUB (Stream A.0); Stream B implements. */
#include "solver/engine.h"

#include <string.h>

void solver_analyze(const struct Board *b, struct Analysis *out) {
  (void)b;
  memset(out, 0, sizeof *out);
  out->best_x = -1;
  out->best_y = -1;
  out->eval = EVAL_START;
}
