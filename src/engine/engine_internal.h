/* engine_internal.h — TEST-ONLY hooks into the engine.
 *
 * NOT part of the installed contract (engine.h). Lives in src/engine/ and is
 * reached only by tests that add src/engine to their include path. Lets the
 * differential test drive the Gaussian-reduction path on small components where
 * the direct enumeration is also valid, so the two can be compared.
 */
#ifndef SOLVER_ENGINE_INTERNAL_H
#define SOLVER_ENGINE_INTERNAL_H

/* When on, enumeration uses the reduction path for EVERY component (including
 * nv <= CAP_VARS), instead of only nv > CAP_VARS. The flag lives on the passed
 * scratch (per-instance, not a global), so the engine stays reentrant; tests
 * set it on their own handle and production never calls this. */
struct SolverScratch;
void solver_test_set_force_reduce(struct SolverScratch* s, bool on);

#endif /* SOLVER_ENGINE_INTERNAL_H */
