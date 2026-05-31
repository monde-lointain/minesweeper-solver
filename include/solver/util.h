/* util.h — header-only leaf clamps shared by the pure (no-SDL) solver modules.
 *
 * static inline so engine.cc and overlay_geom.cc can use them without linking
 * the sibling's util.cc — the no-graphics test targets (engine_test/
 * overlay_test) link solver_lib only. Orthodox C++: free functions, POD, C.
 */
#ifndef SOLVER_UTIL_H
#define SOLVER_UTIL_H

/* Clamp v into [lo, hi]. */
static inline int solver_clampi(int v, int lo, int hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

/* Clamp a probability into [0, 1]. */
static inline long double solver_clamp01(long double v) {
  if (v < 0.0L) {
    return 0.0L;
  }
  if (v > 1.0L) {
    return 1.0L;
  }
  return v;
}

#endif /* SOLVER_UTIL_H */
