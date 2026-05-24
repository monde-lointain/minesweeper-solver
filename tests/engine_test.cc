/* engine_test.cc — STUB (Stream A.0); Stream B writes the real suite. */
#include <string.h>

#include <gtest/gtest.h>

#include "solver/engine.h"

TEST(EngineStub, LinksAndReturnsStart) {
  struct Board b;
  memset(&b, 0, sizeof b);
  b.width = 9;
  b.height = 9;
  b.mines = 10;
  struct Analysis a;
  solver_analyze(&b, &a);
  EXPECT_EQ(a.eval, EVAL_START);
}
