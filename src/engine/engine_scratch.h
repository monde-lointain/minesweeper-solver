/* engine_scratch.h — shared internal scratch data model for the probability
 * engine. NOT part of the public contract (engine.h); lives in src/engine/.
 * Defines the per-analysis working-memory structs + sizing constants used by
 * BOTH engine.cc (pipeline + direct enumeration) and engine_reduce.cc (the
 * Gaussian-reduction path), so the reduction solver can live in its own
 * translation unit. Orthodox C++: POD, plain enums, pointers, C headers.
 */
#ifndef SOLVER_ENGINE_SCRATCH_H
#define SOLVER_ENGINE_SCRATCH_H

#include "minesweeper/types.h" /* BOARD_MAX_CELLS */
#include "rational.h"          /* struct Rat (ReduceScratch matrix) */

enum {
  MAXCELL = BOARD_MAX_CELLS,
  CAP_VARS = 24,      /* direct-enumeration cap (unchanged path) */
  MAX_COMP_VARS = 64, /* Gaussian-reduction exact-solve cap (storage) */
  MAXCOMP = 128,      /* exact-DP component cap */
  MAXFLEN = 256,      /* exact-DP total-frontier-mine cap + 1 */
  MAXVARCON = 8,      /* a covered cell borders <= 8 numbered cells */
  MAX_RED_ROWS = 128, /* constraint rows in the reduction matrix */
  FREE_CAP = 28,      /* max free vars after RREF (search feasibility) */
  /* compact ragged result storage: total exact var-slots bounded by Sigma nv_c
   * <= MAXCELL; total mhat slots by Sigma nv_c*(nv_c+1) <= MAXCELL*(MCV+1). */
  MHAT_FLAT_MAX = MAXCELL * (MAX_COMP_VARS + 1),
  SHAT_FLAT_MAX = MAXCELL + MAXCOMP
};

/* ---- working memory --------------------------------------------------------
 * One per analysis (or per thread). calloc-zeroed on create, which reproduces
 * the original file-scope/BSS semantics: every field is re-initialized before
 * it is read each call (or read only within the [0,len) window written this
 * call), so reuse across calls is safe and a per-call memset is unnecessary.
 *
 * Fields are grouped by pipeline role into POD sub-structs, not packed by
 * alignment: the padding is irrelevant in a ~2.5 MB struct, and grouping aids
 * maintenance. NOLINTNEXTLINE silences the padding analyzer on the sub-structs
 * that mix int and long double.
 */

/* constraint / variable model + union-find over vars */
struct ConstraintModel {
  int var_of_cell[MAXCELL]; /* cell idx -> var id, or -1 */
  int cell_of_var[MAXCELL]; /* var id -> cell idx */
  int nvar;
  int vstate[MAXCELL];     /* per var: VAR_UNKNOWN/SAFE/MINE */
  int con_var[MAXCELL][8]; /* constraint -> var ids of covered neighbors */
  int con_nv[MAXCELL];
  int con_need[MAXCELL]; /* total mines among those vars (= adjacent) */
  int ncon;
  int parent[MAXCELL]; /* union-find over vars */
};

/* component layout: var -> component id, local index, uf-root labels */
struct CompLayout {
  int of_var[MAXCELL]; /* unknown var -> component id, else -1 */
  int local_of_var[MAXCELL];
  int label[MAXCELL]; /* uf root -> component id (build_components scratch) */
};

/* per-component normalized enumeration results, COMPACT RAGGED: per-component
 * data lives in flat arrays indexed by a per-component offset, sized by the
 * total var budget (Sigma nv_c) rather than the dense MAXCOMP x MAX_COMP_VARS
 * worst case. var_off indexes gv/fb_p (one slot per var); shat_off indexes shat
 * (nv+1 slots); mhat_off indexes mhat (nv*(nv+1) slots, stride nv+1). shat/mhat
 * offsets are assigned only to EXACT components (fallback comps can exceed
 * MAX_COMP_VARS and need neither). Access via the res_* helpers below. */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct CompResults {
  int nv[MAXCOMP];
  int var_off[MAXCOMP];  /* -> gv_flat / fb_p_flat */
  int shat_off[MAXCOMP]; /* -> shat_flat (exact only) */
  int mhat_off[MAXCOMP]; /* -> mhat_flat (exact only) */
  bool fallback[MAXCOMP];
  int gv_flat[MAXCELL];           /* local -> global var */
  long double fb_p_flat[MAXCELL]; /* naive prob if fallback */
  long double shat_flat[SHAT_FLAT_MAX];
  long double mhat_flat[MHAT_FLAT_MAX];
};

/* global-combination DP scratch + analyze-local combine buffers */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct CombineDP {
  long double prefix[MAXCOMP + 1][MAXFLEN];
  int prefix_len[MAXCOMP + 1];
  long double suffix[MAXCOMP + 1][MAXFLEN];
  int suffix_len[MAXCOMP + 1];
  int exact_idx[MAXCOMP];  /* compacted list of exact (non-fallback) comps */
  long double oc[MAXFLEN]; /* per-component leave-one-out convolution buffer */
  long double
      fbdist[MAXFLEN]; /* mine-count dist of fallback comps (Poisson-bin) */
  int fbdist_len;
  long double
      wall[MAXFLEN]; /* exact prefix (x) fbdist: full frontier-mine dist */
  long double oc2[MAXFLEN]; /* leave-one-out (x) fbdist */
};

/* enumeration scratch (one component at a time) */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct EnumScratch {
  int nv;
  int ncon;
  int con_lv[MAXCELL][8];
  int con_nlv[MAXCELL];
  int con_need[MAXCELL];
  int con_sum[MAXCELL];
  int con_un[MAXCELL]; /* unassigned vars remaining in the constraint */
  int varcon[MAX_COMP_VARS][MAXVARCON];
  int varcon_n[MAX_COMP_VARS];
  int assign[MAX_COMP_VARS];
  long double sol[MAX_COMP_VARS + 1]; /* per mine-count k: #solutions */
  long double mine[MAX_COMP_VARS]
                  [MAX_COMP_VARS + 1]; /* per (var,k): incidence */
  int nodes;
  bool overflow;
};

/* Gaussian-reduction scratch (one component at a time): rational augmented
 * matrix + free/leading split + the free-var enumeration state (per-row partial
 * value `acc` and pos/neg suffix sums of free-var coefficients for interval
 * pruning). */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct ReduceScratch {
  struct Rat mat[MAX_RED_ROWS][MAX_COMP_VARS + 1]; /* rows x (vars | rhs) */
  int pivcol[MAX_COMP_VARS]; /* leading var (col) of pivot row r */
  bool is_pivot_var[MAX_COMP_VARS];
  int rank;
  int freevar[MAX_COMP_VARS];
  int nfree;
  struct Rat acc[MAX_COMP_VARS]; /* per pivot row: rhs - sum assigned coef*x */
  struct Rat posSuf[MAX_COMP_VARS][MAX_COMP_VARS + 1]; /* suffix max(coef,0) */
  struct Rat negSuf[MAX_COMP_VARS][MAX_COMP_VARS + 1]; /* suffix min(coef,0) */
  int xfull[MAX_COMP_VARS]; /* current full {0,1} assignment */
};

/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct SolverScratch {
  struct ConstraintModel cm;
  struct CompLayout comp;
  struct CompResults res;
  struct CombineDP dp;
  struct EnumScratch ec;
  struct ReduceScratch rd;
  /* Test-only: force the Gaussian-reduction path even for nv <= CAP_VARS, so
   * the differential test can compare reduced vs direct enumeration on small
   * systems where both are valid. Per-instance (not a global) so the engine
   * stays reentrant; calloc-zeroed to false, and never set in production. */
  bool force_reduce;
};

/* Reduce + enumerate the current component via Gaussian elimination over exact
 * rationals (ec already holds the local constraint model). Fills ec.sol /
 * ec.mine; returns false on fallback. Touches only s->rd and s->ec. Defined in
 * engine_reduce.cc. */
bool enumerate_reduced(struct SolverScratch* s);

#endif /* SOLVER_ENGINE_SCRATCH_H */
