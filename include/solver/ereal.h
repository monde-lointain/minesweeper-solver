/* ereal.h — the probability engine's working real type.
 *
 * Default `double`: matches the public `Analysis.mine_prob` and is sufficient —
 * the engine's intermediates are normalized (solution counts <= node budget;
 * per-cell distributions in [0,1]; binomials carried as
 * dominant-term-normalized weights), so nothing exceeds double range. Override
 * at compile time
 *
 *     -DENGINE_REAL_T="long double" -DENGINE_REAL_MAX=LDBL_MAX
 *
 * to build a higher-exponent-range copy of the SAME engine sources — used by
 * the large-N correctness oracle (tests/engine_largeN_test.cc) to certify that
 * the normalized binomial math agrees with a reference that cannot overflow.
 *
 * Orthodox C++: a single typedef over a C header.
 */
#ifndef SOLVER_EREAL_H
#define SOLVER_EREAL_H

#include <float.h>

/* ENGINE_REAL_LDBL is the single-token selector for the long-double reference
 * build (clean to pass as a bare -D, no embedded space). ENGINE_REAL_T /
 * ENGINE_REAL_MAX may also be overridden directly. */
#if defined(ENGINE_REAL_LDBL)
#define ENGINE_REAL_T long double
#define ENGINE_REAL_MAX LDBL_MAX
#endif

#ifndef ENGINE_REAL_T
#define ENGINE_REAL_T double
#endif

#ifndef ENGINE_REAL_MAX
#define ENGINE_REAL_MAX DBL_MAX
#endif

typedef ENGINE_REAL_T ereal;

#endif /* SOLVER_EREAL_H */
