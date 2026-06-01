/* engine.cc — exact Minesweeper probability engine (Stream B).
 *
 * Pipeline (flags ignored throughout; every covered cell is an unknown):
 *   1. Build constraints from revealed numbers over their covered neighbors.
 *   2. Single-point deduction to a fixpoint (shrinks the CSP, marks forced
 * cells).
 *   3. Split the residual unknown frontier into connected components.
 *   4. Backtrack-enumerate each component: per mine-count k, #solutions and
 *      per-variable mine incidence.
 *   5. Combine components + interior cells under the global mine count via a
 *      leave-one-out convolution DP -> exact per-cell P(mine).
 * Exact enumeration subsumes subset reduction for correctness (a var that is a
 * mine in every solution gets P=1 -> forced_mine, etc.); subset reduction would
 * only shrink components, which the per-component cap + node budget already
 * bound.
 *
 * Orthodox C++: POD, plain enums, pointers, C headers. All working state lives
 * in a heap-allocated struct SolverScratch passed by pointer, so the engine is
 * reentrant: independent solver instances may run in parallel, each with its
 * own scratch (never share one handle across threads).
 */
#include "solver/engine.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rational.h"
#include "solver/util.h"

enum { VAR_UNKNOWN = -1, VAR_SAFE = 0, VAR_MINE = 1 };

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

static const long double EPS = 1e-9L;
static const int NODE_BUDGET = 5000000;
/* Separate, smaller budget for the reduced free-var DFS: structured frontiers
 * finish far under it; high-entropy ones bail fast to fallback (each reduced
 * node costs O(rank) rational ops, so a huge budget would be too slow). */
static const int REDUCED_NODE_BUDGET = 30000;

/* Test-only: force the Gaussian-reduction path even for nv <= CAP_VARS, so the
 * differential test can compare reduced vs direct enumeration on small systems
 * where both are valid. Never set in production. */
static bool g_force_reduce = false;
void solver_test_set_force_reduce(bool on) { g_force_reduce = on; }

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
};

/* Compact-ragged accessors: per-component views into the flat result arrays. */
static inline int* res_gv(struct SolverScratch* s, int c) {
  return &s->res.gv_flat[s->res.var_off[c]];
}
static inline long double* res_fbp(struct SolverScratch* s, int c) {
  return &s->res.fb_p_flat[s->res.var_off[c]];
}
static inline long double* res_shat(struct SolverScratch* s, int c) {
  return &s->res.shat_flat[s->res.shat_off[c]];
}
static inline long double* res_mhat(struct SolverScratch* s, int c, int lv) {
  return &s->res.mhat_flat[s->res.mhat_off[c] + lv * (s->res.nv[c] + 1)];
}

struct SolverScratch* solver_scratch_create(void) {
  return (struct SolverScratch*)calloc(1, sizeof(struct SolverScratch));
}

void solver_scratch_destroy(struct SolverScratch* s) {
  if (s != NULL) {
    free(s);
  }
}

/* ---- helpers --------------------------------------------------------------
 */
static long double binom_ld(int n, int k) {
  if (k < 0 || k > n) {
    return 0.0L;
  }
  int kk = (k < n - k) ? k : n - k;
  long double r = 1.0L;
  for (int i = 0; i < kk; ++i) {
    r = r * (long double)(n - i) / (long double)(i + 1);
  }
  return r;
}

static int uf_find(struct SolverScratch* s, int x) {
  while (s->cm.parent[x] != x) {
    s->cm.parent[x] = s->cm.parent[s->cm.parent[x]];
    x = s->cm.parent[x];
  }
  return x;
}

static void uf_union(struct SolverScratch* s, int a, int b) {
  int ra = uf_find(s, a);
  int rb = uf_find(s, b);
  if (ra != rb) {
    s->cm.parent[ra] = rb;
  }
}

static bool cell_covered(const struct Board* b, int x, int y) {
  return !b->cells[game_index(b, x, y)].revealed;
}

/* Tally a constraint's currently-fixed mines and still-unknown vars over its
 * covered-neighbor var list. */
static void constraint_tally(const struct SolverScratch* s, int ci,
                             int* fixed_mines, int* unknown) {
  int fm = 0;
  int unk = 0;
  for (int j = 0; j < s->cm.con_nv[ci]; ++j) {
    int st = s->cm.vstate[s->cm.con_var[ci][j]];
    if (st == VAR_MINE) {
      ++fm;
    } else if (st == VAR_UNKNOWN) {
      ++unk;
    }
  }
  *fixed_mines = fm;
  *unknown = unk;
}

/* ---- step 1: build constraints + variables --------------------------------
 */
static void build_constraints(const struct Board* b, struct SolverScratch* s) {
  s->cm.nvar = 0;
  s->cm.ncon = 0;
  for (int i = 0; i < b->width * b->height; ++i) {
    s->cm.var_of_cell[i] = -1;
  }
  for (int y = 0; y < b->height; ++y) {
    for (int x = 0; x < b->width; ++x) {
      const struct Cell* c = &b->cells[game_index(b, x, y)];
      if (!c->revealed || c->mine || c->adjacent == 0) {
        continue;
      }
      int vars[8];
      int nv = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) {
            continue;
          }
          int nx = x + dx;
          int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= b->width || ny >= b->height) {
            continue;
          }
          if (!cell_covered(b, nx, ny)) {
            continue;
          }
          int idx = game_index(b, nx, ny);
          if (s->cm.var_of_cell[idx] < 0) {
            s->cm.var_of_cell[idx] = s->cm.nvar;
            s->cm.cell_of_var[s->cm.nvar] = idx;
            s->cm.vstate[s->cm.nvar] = VAR_UNKNOWN;
            ++s->cm.nvar;
          }
          vars[nv++] = s->cm.var_of_cell[idx];
        }
      }
      if (nv == 0) {
        continue;
      }
      for (int j = 0; j < nv; ++j) {
        s->cm.con_var[s->cm.ncon][j] = vars[j];
      }
      s->cm.con_nv[s->cm.ncon] = nv;
      s->cm.con_need[s->cm.ncon] = c->adjacent;
      ++s->cm.ncon;
    }
  }
}

/* ---- step 2: single-point deduction fixpoint ------------------------------
 */
static void deduce(struct SolverScratch* s) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (int ci = 0; ci < s->cm.ncon; ++ci) {
      int fixed_mines = 0;
      int unknown = 0;
      constraint_tally(s, ci, &fixed_mines, &unknown);
      if (unknown == 0) {
        continue;
      }
      int rem = s->cm.con_need[ci] - fixed_mines;
      int set_to = -1;
      if (rem <= 0) {
        set_to = VAR_SAFE;
      } else if (rem >= unknown) {
        set_to = VAR_MINE;
      }
      if (set_to >= 0) {
        for (int j = 0; j < s->cm.con_nv[ci]; ++j) {
          int v = s->cm.con_var[ci][j];
          if (s->cm.vstate[v] == VAR_UNKNOWN) {
            s->cm.vstate[v] = set_to;
            changed = true;
          }
        }
      }
    }
  }
}

/* ---- step 3: components over residual unknown vars ------------------------
 */
static int build_components(struct SolverScratch* s) {
  for (int v = 0; v < s->cm.nvar; ++v) {
    s->cm.parent[v] = v;
  }
  for (int ci = 0; ci < s->cm.ncon; ++ci) {
    int first = -1;
    for (int j = 0; j < s->cm.con_nv[ci]; ++j) {
      int v = s->cm.con_var[ci][j];
      if (s->cm.vstate[v] != VAR_UNKNOWN) {
        continue;
      }
      if (first < 0) {
        first = v;
      } else {
        uf_union(s, first, v);
      }
    }
  }
  for (int v = 0; v < s->cm.nvar; ++v) {
    s->comp.label[v] = -1;
    s->comp.of_var[v] = -1;
  }
  int ncomp = 0;
  for (int v = 0; v < s->cm.nvar; ++v) {
    if (s->cm.vstate[v] != VAR_UNKNOWN) {
      continue;
    }
    int r = uf_find(s, v);
    if (s->comp.label[r] < 0) {
      s->comp.label[r] = ncomp++;
    }
    s->comp.of_var[v] = s->comp.label[r];
  }
  return ncomp;
}

/* ---- step 4: per-component enumeration ------------------------------------
 */

/* Pre-pass: count vars per component and assign compact var offsets + local
 * indices + gv_flat, iterating in var-id order so each component's local
 * numbering matches the original gather order (keeps exact marginals
 * identical). Initializes per-component result bookkeeping. */
static void layout_components(struct SolverScratch* s, int ncomp) {
  for (int c = 0; c < ncomp; ++c) {
    s->res.nv[c] = 0;
    s->res.fallback[c] = false;
    s->res.shat_off[c] = 0;
    s->res.mhat_off[c] = 0;
  }
  for (int v = 0; v < s->cm.nvar; ++v) {
    if (s->cm.vstate[v] == VAR_UNKNOWN && s->comp.of_var[v] >= 0) {
      ++s->res.nv[s->comp.of_var[v]];
    }
  }
  int off = 0;
  for (int c = 0; c < ncomp; ++c) {
    s->res.var_off[c] = off;
    off += s->res.nv[c];
  }
  int fill[MAXCOMP];
  for (int c = 0; c < ncomp; ++c) {
    fill[c] = 0;
  }
  for (int v = 0; v < s->cm.nvar; ++v) {
    if (s->cm.vstate[v] != VAR_UNKNOWN || s->comp.of_var[v] < 0) {
      continue;
    }
    int c = s->comp.of_var[v];
    int lv = fill[c]++;
    s->comp.local_of_var[v] = lv;
    s->res.gv_flat[s->res.var_off[c] + lv] = v;
  }
}

static void enum_dfs(struct SolverScratch* s, int i, int total) {
  if (s->ec.overflow) {
    return;
  }
  if (++s->ec.nodes > NODE_BUDGET) {
    s->ec.overflow = true;
    return;
  }
  if (i == s->ec.nv) {
    s->ec.sol[total] += 1.0L;
    for (int lv = 0; lv < s->ec.nv; ++lv) {
      if (s->ec.assign[lv] == 1) {
        s->ec.mine[lv][total] += 1.0L;
      }
    }
    return;
  }
  for (int val = 0; val <= 1; ++val) {
    bool ok = true;
    for (int t = 0; t < s->ec.varcon_n[i]; ++t) {
      int cj = s->ec.varcon[i][t];
      s->ec.con_sum[cj] += val;
      s->ec.con_un[cj] -= 1;
      bool over = s->ec.con_sum[cj] > s->ec.con_need[cj];
      bool closed_wrong =
          s->ec.con_un[cj] == 0 && s->ec.con_sum[cj] != s->ec.con_need[cj];
      bool cannot_reach =
          s->ec.con_sum[cj] + s->ec.con_un[cj] < s->ec.con_need[cj];
      if (over || closed_wrong || cannot_reach) {
        ok = false;
      }
    }
    if (ok) {
      s->ec.assign[i] = val;
      enum_dfs(s, i + 1, total + val);
    }
    for (int t = 0; t < s->ec.varcon_n[i]; ++t) {
      int cj = s->ec.varcon[i][t];
      s->ec.con_sum[cj] -= val;
      s->ec.con_un[cj] += 1;
    }
  }
}

/* ---- Gaussian-elimination reduction (for CAP_VARS < nv <= MAX_COMP_VARS) ----
 *
 * The direct DFS branches all nv vars; on dense Expert frontiers the search
 * tree blows the node budget. Reduction row-reduces the component's {0,1}
 * constraint system to RREF, then enumerates only the FREE variables (nv-rank),
 * back-substituting leading vars and pruning when they leave {0,1}. The output
 * (ec.sol / ec.mine) is identical in form to the direct path, so the combine DP
 * is untouched. All arithmetic is exact rational; any overflow / infeasible
 * row / too-many-free / node-budget condition aborts to the naive fallback so a
 * wrong marginal is never emitted.
 */

/* Build the rational augmented matrix from the gathered local constraints and
 * reduce to RREF. Fills rd.{pivcol,is_pivot_var,rank,freevar,nfree}. Returns
 * false on overflow, an infeasible (0 = nonzero) row, too many rows/free vars,
 * or a degenerate system. */
static bool reduce_rref(struct SolverScratch* s) {
  int nv = s->ec.nv;
  int nr = s->ec.ncon;
  if (nr > MAX_RED_ROWS) {
    return false;
  }
  for (int r = 0; r < nr; ++r) {
    for (int j = 0; j <= nv; ++j) {
      s->rd.mat[r][j] = rat_from_i64(0);
    }
    for (int t = 0; t < s->ec.con_nlv[r]; ++t) {
      s->rd.mat[r][s->ec.con_lv[r][t]] = rat_from_i64(1);
    }
    s->rd.mat[r][nv] = rat_from_i64(s->ec.con_need[r]);
  }
  for (int j = 0; j < nv; ++j) {
    s->rd.is_pivot_var[j] = false;
  }
  int rank = 0;
  for (int col = 0; col < nv && rank < nr; ++col) {
    int piv = -1;
    for (int r = rank; r < nr; ++r) {
      if (!rat_is_zero(s->rd.mat[r][col])) {
        piv = r;
        break;
      }
    }
    if (piv < 0) {
      continue; /* free column */
    }
    if (piv != rank) {
      for (int j = 0; j <= nv; ++j) {
        struct Rat tmp = s->rd.mat[rank][j];
        s->rd.mat[rank][j] = s->rd.mat[piv][j];
        s->rd.mat[piv][j] = tmp;
      }
    }
    struct Rat pivval = s->rd.mat[rank][col];
    for (int j = 0; j <= nv; ++j) {
      s->rd.mat[rank][j] = rat_div(s->rd.mat[rank][j], pivval);
      if (!rat_ok(s->rd.mat[rank][j])) {
        return false;
      }
    }
    for (int r = 0; r < nr; ++r) {
      if (r == rank) {
        continue;
      }
      struct Rat f = s->rd.mat[r][col];
      if (rat_is_zero(f)) {
        continue;
      }
      for (int j = 0; j <= nv; ++j) {
        struct Rat prod = rat_mul(f, s->rd.mat[rank][j]);
        s->rd.mat[r][j] = rat_sub(s->rd.mat[r][j], prod);
        if (!rat_ok(s->rd.mat[r][j])) {
          return false;
        }
      }
    }
    s->rd.pivcol[rank] = col;
    s->rd.is_pivot_var[col] = true;
    ++rank;
  }
  s->rd.rank = rank;
  /* consistency: any all-zero-coefficient row must have zero rhs */
  for (int r = rank; r < nr; ++r) {
    bool allzero = true;
    for (int j = 0; j < nv; ++j) {
      if (!rat_is_zero(s->rd.mat[r][j])) {
        allzero = false;
        break;
      }
    }
    if (allzero && !rat_is_zero(s->rd.mat[r][nv])) {
      return false; /* 0 = nonzero: infeasible */
    }
  }
  s->rd.nfree = 0;
  for (int j = 0; j < nv; ++j) {
    if (!s->rd.is_pivot_var[j]) {
      s->rd.freevar[s->rd.nfree++] = j;
    }
  }
  if (s->rd.nfree > FREE_CAP) {
    return false;
  }
  return true;
}

/* Precompute, per pivot row, the suffix sums (over free-var order) of the
 * positive and negative parts of that row's free-var coefficients, used for
 * interval pruning. Initialize acc[r] = rhs of pivot row r. Returns false on
 * overflow. */
static bool reduce_prep_enum(struct SolverScratch* s) {
  int nv = s->ec.nv;
  for (int r = 0; r < s->rd.rank; ++r) {
    s->rd.acc[r] = s->rd.mat[r][nv];
    s->rd.posSuf[r][s->rd.nfree] = rat_from_i64(0);
    s->rd.negSuf[r][s->rd.nfree] = rat_from_i64(0);
    for (int d = s->rd.nfree - 1; d >= 0; --d) {
      struct Rat coef = s->rd.mat[r][s->rd.freevar[d]];
      struct Rat pos = rat_cmp_i64(coef, 0) > 0 ? coef : rat_from_i64(0);
      struct Rat neg = rat_cmp_i64(coef, 0) < 0 ? coef : rat_from_i64(0);
      s->rd.posSuf[r][d] = rat_add(s->rd.posSuf[r][d + 1], pos);
      s->rd.negSuf[r][d] = rat_add(s->rd.negSuf[r][d + 1], neg);
      if (!rat_ok(s->rd.posSuf[r][d]) || !rat_ok(s->rd.negSuf[r][d])) {
        return false;
      }
    }
  }
  return true;
}

/* Accumulate the current full {0,1} assignment into ec.sol / ec.mine. */
static void reduced_accumulate(struct SolverScratch* s) {
  int total = 0;
  for (int lv = 0; lv < s->ec.nv; ++lv) {
    total += s->rd.xfull[lv];
  }
  s->ec.sol[total] += 1.0L;
  for (int lv = 0; lv < s->ec.nv; ++lv) {
    if (s->rd.xfull[lv] == 1) {
      s->ec.mine[lv][total] += 1.0L;
    }
  }
}

/* DFS over free variable position d in {0,1}, maintaining acc[r] (pivot-row
 * partial value) and pruning rows whose leading var can no longer land in
 * {0,1}. At the leaf, each pivot row's leading var = acc[r] must be exactly 0
 * or 1. Any rational overflow sets ec.overflow (-> fallback). */
static void reduced_dfs(struct SolverScratch* s, int d) {
  if (s->ec.overflow) {
    return;
  }
  if (++s->ec.nodes > REDUCED_NODE_BUDGET) {
    s->ec.overflow = true;
    return;
  }
  if (d == s->rd.nfree) {
    for (int r = 0; r < s->rd.rank; ++r) {
      if (rat_eq_i64(s->rd.acc[r], 0)) {
        s->rd.xfull[s->rd.pivcol[r]] = 0;
      } else if (rat_eq_i64(s->rd.acc[r], 1)) {
        s->rd.xfull[s->rd.pivcol[r]] = 1;
      } else {
        return; /* leading var not in {0,1}: not a solution */
      }
    }
    reduced_accumulate(s);
    return;
  }
  int fv = s->rd.freevar[d];
  for (int val = 0; val <= 1; ++val) {
    s->rd.xfull[fv] = val;
    if (val == 1) {
      for (int r = 0; r < s->rd.rank; ++r) {
        struct Rat coef = s->rd.mat[r][fv];
        if (rat_is_zero(coef)) {
          continue;
        }
        s->rd.acc[r] = rat_sub(s->rd.acc[r], coef);
        if (!rat_ok(s->rd.acc[r])) {
          s->ec.overflow = true;
          return;
        }
      }
    }
    bool ok = true;
    for (int r = 0; r < s->rd.rank && ok; ++r) {
      struct Rat lo = rat_sub(s->rd.acc[r], s->rd.posSuf[r][d + 1]);
      struct Rat hi = rat_sub(s->rd.acc[r], s->rd.negSuf[r][d + 1]);
      if (!rat_ok(lo) || !rat_ok(hi)) {
        s->ec.overflow = true;
        return;
      }
      bool c0 = rat_cmp_i64(lo, 0) <= 0 && rat_cmp_i64(hi, 0) >= 0;
      bool c1 = rat_cmp_i64(lo, 1) <= 0 && rat_cmp_i64(hi, 1) >= 0;
      if (!c0 && !c1) {
        ok = false;
      }
    }
    if (ok) {
      reduced_dfs(s, d + 1);
      if (s->ec.overflow) {
        return;
      }
    }
    if (val == 1) {
      for (int r = 0; r < s->rd.rank; ++r) {
        struct Rat coef = s->rd.mat[r][fv];
        if (!rat_is_zero(coef)) {
          s->rd.acc[r] = rat_add(s->rd.acc[r], coef);
        }
      }
    }
  }
}

/* Reduce + enumerate the current component (ec already holds the local
 * constraint model). Fills ec.sol / ec.mine. Returns false on fallback. */
static bool enumerate_reduced(struct SolverScratch* s) {
  if (!reduce_rref(s)) {
    return false;
  }
  if (!reduce_prep_enum(s)) {
    return false;
  }
  s->ec.nodes = 0;
  s->ec.overflow = false;
  reduced_dfs(s, 0);
  return !s->ec.overflow;
}

/* Build the local model for component `comp` (vars + local indices precomputed
 * by layout_components) and enumerate it directly. Returns false on fallback
 * (over the direct cap / node budget / infeasible), true on exact success (and
 * then reserves this component's compact shat/mhat slots, advancing the running
 * offsets). The Gaussian-reduction path for CAP_VARS < nv <= MAX_COMP_VARS is
 * added in sub-step 2. */
static bool enumerate_component(struct SolverScratch* s, int comp,
                                int* shat_used, int* mhat_used) {
  int nv = s->res.nv[comp];
  if (nv > MAX_COMP_VARS) {
    return false; /* too big to store exactly: fallback */
  }
  s->ec.nv = nv;
  /* gather local constraints */
  s->ec.ncon = 0;
  for (int lv = 0; lv < nv; ++lv) {
    s->ec.varcon_n[lv] = 0;
  }
  for (int ci = 0; ci < s->cm.ncon; ++ci) {
    int first_unknown = -1;
    for (int j = 0; j < s->cm.con_nv[ci]; ++j) {
      int v = s->cm.con_var[ci][j];
      if (s->cm.vstate[v] == VAR_UNKNOWN) {
        first_unknown = v;
        break;
      }
    }
    if (first_unknown < 0 || s->comp.of_var[first_unknown] != comp) {
      continue;
    }
    int fixed_mines = 0;
    int nlv = 0;
    for (int j = 0; j < s->cm.con_nv[ci]; ++j) {
      int v = s->cm.con_var[ci][j];
      if (s->cm.vstate[v] == VAR_MINE) {
        ++fixed_mines;
      } else if (s->cm.vstate[v] == VAR_UNKNOWN) {
        int lv = s->comp.local_of_var[v];
        s->ec.con_lv[s->ec.ncon][nlv] = lv;
        s->ec.varcon[lv][s->ec.varcon_n[lv]++] = s->ec.ncon;
        ++nlv;
      }
    }
    s->ec.con_nlv[s->ec.ncon] = nlv;
    s->ec.con_need[s->ec.ncon] = s->cm.con_need[ci] - fixed_mines;
    ++s->ec.ncon;
  }
  /* init enumeration state */
  for (int k = 0; k <= nv; ++k) {
    s->ec.sol[k] = 0.0L;
  }
  for (int lv = 0; lv < nv; ++lv) {
    for (int k = 0; k <= nv; ++k) {
      s->ec.mine[lv][k] = 0.0L;
    }
  }
  for (int cj = 0; cj < s->ec.ncon; ++cj) {
    s->ec.con_sum[cj] = 0;
    s->ec.con_un[cj] = s->ec.con_nlv[cj];
  }
  if (nv > CAP_VARS || g_force_reduce) {
    if (!enumerate_reduced(s)) { /* Gaussian reduction + free-var DFS */
      return false;
    }
  } else {
    s->ec.nodes = 0;
    s->ec.overflow = false;
    enum_dfs(s, 0, 0); /* direct: branch all vars (unchanged path) */
    if (s->ec.overflow) {
      return false;
    }
  }
  /* normalize into compact comp tables */
  long double total = 0.0L;
  for (int k = 0; k <= nv; ++k) {
    total += s->ec.sol[k];
  }
  if (total <= 0.0L) {
    return false; /* infeasible component (shouldn't happen): fallback */
  }
  s->res.fallback[comp] = false;
  s->res.shat_off[comp] = *shat_used;
  *shat_used += nv + 1;
  s->res.mhat_off[comp] = *mhat_used;
  *mhat_used += nv * (nv + 1);
  long double* shat = res_shat(s, comp);
  for (int k = 0; k <= nv; ++k) {
    shat[k] = s->ec.sol[k] / total;
  }
  for (int lv = 0; lv < nv; ++lv) {
    long double* mh = res_mhat(s, comp, lv);
    for (int k = 0; k <= nv; ++k) {
      mh[k] = s->ec.mine[lv][k] / total;
    }
  }
  return true;
}

/* Naive per-var fallback for an over-budget component. Models the first
 * min(nv, CAP_VARS) vars (slots reserved by layout_components, in var-id
 * order); the remainder stay at the deduced default and are clamped by Fix A.
 */
static void fallback_component(struct SolverScratch* s, int comp) {
  s->res.fallback[comp] = true;
  int nv = s->res.nv[comp];
  int use = nv < CAP_VARS ? nv : CAP_VARS;
  s->res.nv[comp] = use;
  int* gv = res_gv(s, comp);
  long double* fbp = res_fbp(s, comp);
  for (int lv = 0; lv < use; ++lv) {
    int cell = s->cm.cell_of_var[gv[lv]];
    long double sum = 0.0L;
    int cnt = 0;
    for (int ci = 0; ci < s->cm.ncon; ++ci) {
      int unknown = 0;
      int fixed_mines = 0;
      constraint_tally(s, ci, &fixed_mines, &unknown);
      bool has = false;
      for (int j = 0; j < s->cm.con_nv[ci]; ++j) {
        if (s->cm.cell_of_var[s->cm.con_var[ci][j]] == cell) {
          has = true;
          break;
        }
      }
      if (has && unknown > 0) {
        int rem = s->cm.con_need[ci] - fixed_mines;
        sum += solver_clamp01((long double)rem / (long double)unknown);
        ++cnt;
      }
    }
    fbp[lv] = cnt > 0 ? sum / (long double)cnt : 0.5L;
  }
}

/* ---- step 5: global combination -------------------------------------------
 */
static void conv(const long double* a, int la, const long double* b, int lb,
                 long double* out, int* lout) {
  int n = la + lb - 1;
  for (int i = 0; i < n; ++i) {
    out[i] = 0.0L;
  }
  for (int i = 0; i < la; ++i) {
    if (a[i] == 0.0L) {
      continue;
    }
    for (int j = 0; j < lb; ++j) {
      out[i + j] += a[i] * b[j];
    }
  }
  *lout = n;
}

/* Transient values shared between analyze sub-steps (call-local, not part of
 * the persistent scratch). */
struct AnalyzeCtx {
  int ncells;
  int known_mines;
  int ncomp;
  bool exact_ok;
  int interior_n;
  int r_eff;
  int nexact;
  long double zsum;
  long double interior_prob;
};

/* Terminal verdict (won/lost). Returns true if `out` was fully written. */
static bool analyze_terminal(const struct Board* b, struct Analysis* out) {
  if (b->status == GAME_WON) {
    out->eval = EVAL_SOLVED;
    return true;
  }
  if (b->status == GAME_LOST) {
    out->eval = EVAL_LOST;
    return true;
  }
  return false;
}

/* EVAL_START: nothing revealed yet -> uniform density, suggest a corner. */
static void analyze_start(const struct Board* b, struct Analysis* out) {
  int ncells = b->width * b->height;
  long double uniform =
      ncells > 0 ? (long double)b->mines / (long double)ncells : 0.0L;
  for (int i = 0; i < ncells; ++i) {
    if (!b->cells[i].revealed) {
      out->cells[i].mine_prob = (double)uniform;
      out->cells[i].is_frontier = false;
    }
  }
  out->best_x = 0;
  out->best_y = 0;
  out->best_prob = (double)uniform;
  out->interior_prob = (double)uniform;
  out->interior_count = ncells;
  out->eval = EVAL_START;
}

static int count_known_mines(struct SolverScratch* s) {
  int known = 0;
  for (int v = 0; v < s->cm.nvar; ++v) {
    if (s->cm.vstate[v] == VAR_MINE) {
      ++known;
    }
  }
  return known;
}

/* interior cells: covered, not a frontier variable. */
static int count_interior(const struct Board* b, struct SolverScratch* s) {
  int ncells = b->width * b->height;
  int n = 0;
  for (int i = 0; i < ncells; ++i) {
    if (!b->cells[i].revealed && s->cm.var_of_cell[i] < 0) {
      ++n;
    }
  }
  return n;
}

/* Enumerate every component (exact, else naive fallback). May clear *exact_ok
 * if the exact frontier overflows MAXFLEN; accumulates fallback mine mass. */
static void enumerate_all(struct SolverScratch* s, int ncomp, bool* exact_ok,
                          long double* fallback_expected) {
  *fallback_expected = 0.0L;
  if (!*exact_ok) {
    return;
  }
  layout_components(s, ncomp);
  int shat_used = 0;
  int mhat_used = 0;
  for (int c = 0; c < ncomp; ++c) {
    if (!enumerate_component(s, c, &shat_used, &mhat_used)) {
      fallback_component(s, c);
    }
  }
  /* The combine DP now convolves BOTH exact-component distributions and the
   * fallback components' Poisson-binomial (fbdist), so the total frontier-var
   * count must fit MAXFLEN. fallback_expected is still the rounded point mean
   * used only by the crude no-DP interior fallback. */
  int exact_unknown = 0;
  int fb_unknown = 0;
  for (int c = 0; c < ncomp; ++c) {
    if (!s->res.fallback[c]) {
      exact_unknown += s->res.nv[c];
    } else {
      fb_unknown += s->res.nv[c];
      long double* fbp = res_fbp(s, c);
      for (int lv = 0; lv < s->res.nv[c]; ++lv) {
        *fallback_expected += fbp[lv];
      }
    }
  }
  if (exact_unknown + fb_unknown >= MAXFLEN) {
    *exact_ok = false;
  }
}

/* Build the mine-count distribution of all (modeled) fallback-component vars as
 * a Poisson-binomial over their naive per-var probabilities. Identity ([1.0])
 * when there are no fallback components. Result in s->dp.fbdist[0..fbdist_len).
 */
static void build_fbdist(struct SolverScratch* s, int ncomp) {
  s->dp.fbdist[0] = 1.0L;
  s->dp.fbdist_len = 1;
  for (int c = 0; c < ncomp; ++c) {
    if (!s->res.fallback[c]) {
      continue;
    }
    long double* fbp = res_fbp(s, c);
    for (int lv = 0; lv < s->res.nv[c]; ++lv) {
      long double bern[2];
      bern[0] = 1.0L - fbp[lv];
      bern[1] = fbp[lv];
      int outlen = 0;
      conv(s->dp.fbdist, s->dp.fbdist_len, bern, 2, s->dp.wall, &outlen);
      for (int i = 0; i < outlen; ++i) {
        s->dp.fbdist[i] = s->dp.wall[i];
      }
      s->dp.fbdist_len = outlen;
    }
  }
}

/* Combine exact components + fallback distribution + interior under the global
 * mine count: prefix/suffix convolution DP -> ctx->{r_eff, nexact, zsum,
 * interior_prob}. The exact DP shares the FULL remaining budget with the
 * interior; fallback components enter as a distribution (fbdist), not a rounded
 * mean, so the budget is never collapsed (which previously forced spurious
 * 0/1). Only the crude no-DP interior fallback keeps the point estimate. */
static void compute_interior_prob(const struct Board* b,
                                  struct SolverScratch* s,
                                  struct AnalyzeCtx* ctx,
                                  long double fallback_expected) {
  int rem_mines = b->mines - ctx->known_mines;
  int r_eff_approx = rem_mines - (int)llroundl(fallback_expected);
  r_eff_approx = (r_eff_approx < 0) ? 0 : r_eff_approx;
  /* exact DP: full budget (fbdist carries the fallback mines distributionally);
   * no-DP fallback: keep the rounded point estimate. */
  ctx->r_eff = ctx->exact_ok ? rem_mines : r_eff_approx;

  /* Build prefix/suffix convolutions over EXACT components only. */
  int nexact = 0;
  if (ctx->exact_ok) {
    for (int c = 0; c < ctx->ncomp; ++c) {
      if (!s->res.fallback[c]) {
        s->dp.exact_idx[nexact++] = c;
      }
    }
  }
  ctx->nexact = nexact;

  long double zsum = 0.0L;
  long double interior_num = 0.0L;
  if (ctx->exact_ok) {
    build_fbdist(s, ctx->ncomp);
    s->dp.prefix[0][0] = 1.0L;
    s->dp.prefix_len[0] = 1;
    for (int e = 0; e < nexact; ++e) {
      int c = s->dp.exact_idx[e];
      conv(s->dp.prefix[e], s->dp.prefix_len[e], res_shat(s, c),
           s->res.nv[c] + 1, s->dp.prefix[e + 1], &s->dp.prefix_len[e + 1]);
    }
    s->dp.suffix[nexact][0] = 1.0L;
    s->dp.suffix_len[nexact] = 1;
    for (int e = nexact - 1; e >= 0; --e) {
      int c = s->dp.exact_idx[e];
      conv(res_shat(s, c), s->res.nv[c] + 1, s->dp.suffix[e + 1],
           s->dp.suffix_len[e + 1], s->dp.suffix[e], &s->dp.suffix_len[e]);
    }
    /* Wall = (exact prefix) (x) fbdist = full frontier-mine distribution. */
    int wall_len = 0;
    conv(s->dp.prefix[nexact], s->dp.prefix_len[nexact], s->dp.fbdist,
         s->dp.fbdist_len, s->dp.wall, &wall_len);
    /* zsum = sum_F Wall[F] * C(interior_n, r_eff - F) */
    for (int f = 0; f < wall_len; ++f) {
      long double w = s->dp.wall[f];
      if (w == 0.0L) {
        continue;
      }
      long double bcoef = binom_ld(ctx->interior_n, ctx->r_eff - f);
      zsum += w * bcoef;
      interior_num += w * bcoef * (long double)(ctx->r_eff - f);
    }
  }
  ctx->zsum = zsum;

  long double interior_prob = 0.0L;
  if (ctx->exact_ok && zsum > 0.0L && ctx->interior_n > 0) {
    interior_prob = interior_num / (zsum * (long double)ctx->interior_n);
  } else if (ctx->interior_n > 0) {
    /* crude no-DP path: uniform remaining density (point estimate). */
    interior_prob = solver_clamp01((long double)r_eff_approx /
                                   (long double)ctx->interior_n);
  }
  ctx->interior_prob = interior_prob;
}

/* Write every covered cell's P(mine): deduced/interior baseline, then exact
 * component marginals, then naive fallback components. */
static void write_cell_probs(const struct Board* b, struct Analysis* out,
                             struct SolverScratch* s,
                             const struct AnalyzeCtx* ctx) {
  int ncells = ctx->ncells;
  /* out->cells was already zeroed by memset in solver_analyze. */

  /* deduced + interior */
  for (int i = 0; i < ncells; ++i) {
    if (b->cells[i].revealed) {
      continue;
    }
    int v = s->cm.var_of_cell[i];
    if (v < 0) {
      out->cells[i].is_frontier = false;
      out->cells[i].mine_prob = (double)ctx->interior_prob;
      continue;
    }
    out->cells[i].is_frontier = true;
    if (s->cm.vstate[v] == VAR_SAFE) {
      out->cells[i].mine_prob = 0.0;
    } else if (s->cm.vstate[v] == VAR_MINE) {
      out->cells[i].mine_prob = 1.0;
    } else {
      out->cells[i].mine_prob = 0.5; /* overwritten below if exact */
    }
  }

  /* exact component marginals: P(v) = (sum_k mhat[v][k]*A_c[k]) / zsum */
  if (ctx->exact_ok && ctx->zsum > 0.0L) {
    for (int e = 0; e < ctx->nexact; ++e) {
      int c = s->dp.exact_idx[e];
      /* O_c = prefix[e] (x) suffix[e+1] */
      int oclen = 0;
      conv(s->dp.prefix[e], s->dp.prefix_len[e], s->dp.suffix[e + 1],
           s->dp.suffix_len[e + 1], s->dp.oc, &oclen);
      /* fold in the fallback distribution so c's complement covers ALL other
       * frontier mines (other exact comps + fallback comps). */
      int oc2len = 0;
      conv(s->dp.oc, oclen, s->dp.fbdist, s->dp.fbdist_len, s->dp.oc2, &oc2len);
      /* A_c[k] = sum_t oc2[t] * C(interior_n, r_eff - k - t) */
      long double ac[MAX_COMP_VARS + 1];
      for (int k = 0; k <= s->res.nv[c]; ++k) {
        long double sm = 0.0L;
        for (int t = 0; t < oc2len; ++t) {
          if (s->dp.oc2[t] == 0.0L) {
            continue;
          }
          sm += s->dp.oc2[t] * binom_ld(ctx->interior_n, ctx->r_eff - k - t);
        }
        ac[k] = sm;
      }
      int* gv = res_gv(s, c);
      for (int lv = 0; lv < s->res.nv[c]; ++lv) {
        long double* mh = res_mhat(s, c, lv);
        long double num = 0.0L;
        for (int k = 0; k <= s->res.nv[c]; ++k) {
          num += mh[k] * ac[k];
        }
        int cell = s->cm.cell_of_var[gv[lv]];
        out->cells[cell].mine_prob = (double)solver_clamp01(num / ctx->zsum);
      }
    }
  }

  /* fallback components: naive probs directly */
  for (int c = 0; c < ctx->ncomp; ++c) {
    if (!ctx->exact_ok || s->res.fallback[c]) {
      int* gv = res_gv(s, c);
      long double* fbp = res_fbp(s, c);
      for (int lv = 0; lv < s->res.nv[c]; ++lv) {
        int cell = s->cm.cell_of_var[gv[lv]];
        out->cells[cell].mine_prob = (double)fbp[lv];
      }
    }
  }

  /* Approximate paths must never masquerade as proofs: a P(mine) of exactly 0/1
   * from an APPROXIMATE source (naive fallback, or the crude no-DP path) is not
   * a real proof, so clamp it away from {0,1} — else pick_best_move marks it
   * forced and the policy walks into a mispriced mine.
   *
   * This is PER-COMPONENT (a cell safe in all local solutions is safe in all
   * global solutions, so an EXACT component's 0/1 IS a proof and must be kept;
   * the Gaussian reduction makes mixed exact+fallback boards common, where a
   * board-global clamp would discard the reduction's hard-won forced cells).
   * Clamp only: (a) cells in a fallback component, or (b) everything when the
   * exact DP did not run (!exact_ok). Single-point-deduced cells and exact /
   * interior cells keep their exact 0/1. */
  const double kApproxLo = 1e-6; /* >> EPS (1e-9): never reads as forced */
  for (int i = 0; i < ncells; ++i) {
    if (b->cells[i].revealed) {
      continue;
    }
    int v = s->cm.var_of_cell[i];
    if (v >= 0 &&
        (s->cm.vstate[v] == VAR_SAFE || s->cm.vstate[v] == VAR_MINE)) {
      continue; /* genuine single-point proof */
    }
    bool approx;
    if (!ctx->exact_ok) {
      approx = true; /* no DP ran: whole board approximate */
    } else if (v >= 0) {
      int comp =
          s->comp.of_var[v]; /* frontier cell: approximate iff fallback */
      approx = comp >= 0 && s->res.fallback[comp];
    } else {
      approx = false; /* interior under the exact DP: trustworthy */
    }
    if (!approx) {
      continue;
    }
    double p = out->cells[i].mine_prob;
    if (p < kApproxLo) {
      p = kApproxLo;
    } else if (p > 1.0 - kApproxLo) {
      p = 1.0 - kApproxLo;
    }
    out->cells[i].mine_prob = p;
  }
}

/* Set forced flags, pick the lowest-risk move (row-major first on ties), and
 * derive the eval verdict. */
static void pick_best_move(const struct Board* b, struct Analysis* out,
                           const struct AnalyzeCtx* ctx) {
  double best = 2.0;
  int best_x = -1;
  int best_y = -1;
  for (int y = 0; y < b->height; ++y) {
    for (int x = 0; x < b->width; ++x) {
      int i = game_index(b, x, y);
      if (b->cells[i].revealed) {
        continue;
      }
      double p = out->cells[i].mine_prob;
      if ((long double)p < EPS) {
        out->cells[i].forced_safe = true;
      }
      if ((long double)p > 1.0L - EPS) {
        out->cells[i].forced_mine = true;
      }
      if (p < best) {
        best = p;
        best_x = x;
        best_y = y;
      }
    }
  }

  out->interior_count = ctx->interior_n;
  out->interior_prob = (double)ctx->interior_prob;
  out->best_x = best_x;
  out->best_y = best_y;
  out->best_prob = best_x >= 0 ? best : 0.0;
  if (best_x < 0) {
    out->eval = EVAL_SOLVED;
  } else if ((long double)best < EPS) {
    out->eval = EVAL_SAFE;
  } else {
    out->eval = EVAL_GUESS;
  }
}

/* ---- public entry ---------------------------------------------------------
 */
void solver_analyze(const struct Board* b, struct Analysis* out,
                    struct SolverScratch* s) {
  memset(out, 0, sizeof *out);
  out->best_x = -1;
  out->best_y = -1;

  if (analyze_terminal(b, out)) {
    return;
  }
  if (b->revealed_count == 0) {
    analyze_start(b, out);
    return;
  }

  build_constraints(b, s);
  deduce(s);

  struct AnalyzeCtx ctx;
  ctx.ncells = b->width * b->height;
  ctx.known_mines = count_known_mines(s);
  ctx.ncomp = build_components(s);
  ctx.exact_ok = (ctx.ncomp <= MAXCOMP);
  ctx.interior_n = count_interior(b, s);

  long double fallback_expected = 0.0L;
  enumerate_all(s, ctx.ncomp, &ctx.exact_ok, &fallback_expected);

  compute_interior_prob(b, s, &ctx, fallback_expected);
  write_cell_probs(b, out, s, &ctx);
  pick_best_move(b, out, &ctx);
}

/* ---- info gain (paper's Inf(x)) -------------------------------------------
 *
 * For a candidate cell, how many OTHER frontier cells become provably safe or
 * mine if the candidate is assumed safe. Re-enumerate the candidate's component
 * with the candidate excluded (== pinned to 0) and count variables that become
 * invariable (restricted marginal in {0,1}) and were not invariable before.
 *
 * The pinned re-solve reuses the same direct/reduced dispatch as
 * enumerate_component but writes only the transient ec/rd scratch, so
 * s->res/s->cm/s->comp stay intact and this is safe to call once per candidate.
 * Soundness: a variable fixed in every local solution (candidate pinned) is
 * fixed in every global solution (the per-component Fix-A argument), so each
 * counted cell is a real future deduction. Returns 0 on overflow / infeasible /
 * a degenerate component. */
static int component_infogain(struct SolverScratch* s, int comp, int pin_var) {
  int nv_full = s->res.nv[comp];
  int pin_local = s->comp.local_of_var[pin_var];
  int ec_of_orig[MAX_COMP_VARS]; /* original local -> ec local (-1 for pin) */
  int ec2orig[MAX_COMP_VARS];    /* ec local -> original local */
  int m = 0;
  for (int ol = 0; ol < nv_full; ++ol) {
    if (ol == pin_local) {
      ec_of_orig[ol] = -1;
      continue;
    }
    ec_of_orig[ol] = m;
    ec2orig[m] = ol;
    ++m;
  }
  if (m == 0) {
    return 0;
  }
  /* Build the local model over the component's unknown vars, excluding pin. */
  s->ec.nv = m;
  s->ec.ncon = 0;
  for (int lv = 0; lv < m; ++lv) {
    s->ec.varcon_n[lv] = 0;
  }
  for (int ci = 0; ci < s->cm.ncon; ++ci) {
    int first_unknown = -1;
    for (int j = 0; j < s->cm.con_nv[ci]; ++j) {
      int v = s->cm.con_var[ci][j];
      if (s->cm.vstate[v] == VAR_UNKNOWN) {
        first_unknown = v;
        break;
      }
    }
    if (first_unknown < 0 || s->comp.of_var[first_unknown] != comp) {
      continue;
    }
    int fixed_mines = 0;
    int nlv = 0;
    for (int j = 0; j < s->cm.con_nv[ci]; ++j) {
      int v = s->cm.con_var[ci][j];
      if (s->cm.vstate[v] == VAR_MINE) {
        ++fixed_mines;
      } else if (s->cm.vstate[v] == VAR_UNKNOWN && v != pin_var) {
        int lv = ec_of_orig[s->comp.local_of_var[v]];
        s->ec.con_lv[s->ec.ncon][nlv] = lv;
        s->ec.varcon[lv][s->ec.varcon_n[lv]++] = s->ec.ncon;
        ++nlv;
      }
    }
    int need = s->cm.con_need[ci] - fixed_mines; /* pin (=0) adds no mines */
    if (nlv == 0) {
      if (need != 0) {
        return 0; /* pinning the candidate safe is infeasible here */
      }
      continue; /* redundant 0 = 0 row */
    }
    s->ec.con_nlv[s->ec.ncon] = nlv;
    s->ec.con_need[s->ec.ncon] = need;
    ++s->ec.ncon;
  }
  for (int k = 0; k <= m; ++k) {
    s->ec.sol[k] = 0.0L;
  }
  for (int lv = 0; lv < m; ++lv) {
    for (int k = 0; k <= m; ++k) {
      s->ec.mine[lv][k] = 0.0L;
    }
  }
  for (int cj = 0; cj < s->ec.ncon; ++cj) {
    s->ec.con_sum[cj] = 0;
    s->ec.con_un[cj] = s->ec.con_nlv[cj];
  }
  if (m > CAP_VARS) {
    if (!enumerate_reduced(s)) {
      return 0;
    }
  } else {
    s->ec.nodes = 0;
    s->ec.overflow = false;
    enum_dfs(s, 0, 0);
    if (s->ec.overflow) {
      return 0;
    }
  }
  long double total = 0.0L;
  for (int k = 0; k <= m; ++k) {
    total += s->ec.sol[k];
  }
  if (total <= 0.0L) {
    return 0;
  }
  int gain = 0;
  for (int lv = 0; lv < m; ++lv) {
    long double rm = 0.0L;
    for (int k = 0; k <= m; ++k) {
      rm += s->ec.mine[lv][k];
    }
    rm /= total;
    bool restr_inv = rm < EPS || rm > 1.0L - EPS;
    if (!restr_inv) {
      continue; /* still uncertain after the guess */
    }
    long double* mh = res_mhat(s, comp, ec2orig[lv]);
    long double om = 0.0L;
    for (int k = 0; k <= nv_full; ++k) {
      om += mh[k];
    }
    bool orig_inv = om < EPS || om > 1.0L - EPS;
    if (!orig_inv) {
      ++gain; /* newly forced by assuming the candidate safe */
    }
  }
  return gain;
}

/* Only competitive guesses are worth pricing: a policy guesses near the minimum
 * risk, so info_gain is computed solely for frontier cells within this band of
 * best_prob. Must stay >= the bench's guess band (HEUR_BAND) so every cell a
 * policy considers is covered; widening it only costs time, never correctness.
 */
static const double INFOGAIN_BAND = 0.05;

/* Fill info_gain for non-forced frontier cells near the minimum risk in an exact
 * component. Called only at EVAL_GUESS; clobbers only the transient ec/rd
 * scratch. */
static void compute_infogain(const struct Board* b, struct Analysis* out,
                             struct SolverScratch* s) {
  int ncells = b->width * b->height;
  double thresh = out->best_prob + INFOGAIN_BAND;
  for (int i = 0; i < ncells; ++i) {
    if (b->cells[i].revealed) {
      continue;
    }
    if (!out->cells[i].is_frontier || out->cells[i].forced_mine) {
      continue;
    }
    if (out->cells[i].mine_prob > thresh) {
      continue; /* not a competitive guess */
    }
    int v = s->cm.var_of_cell[i];
    if (v < 0 || s->cm.vstate[v] != VAR_UNKNOWN) {
      continue;
    }
    int comp = s->comp.of_var[v];
    if (comp < 0 || s->res.fallback[comp]) {
      continue; /* no exact local solution set: no reliable info gain */
    }
    out->cells[i].info_gain = component_infogain(s, comp, v);
  }
}

void solver_analyze_infogain(const struct Board* b, struct Analysis* out,
                             struct SolverScratch* s) {
  solver_analyze(b, out, s);
  if (out->eval == EVAL_GUESS) {
    compute_infogain(b, out, s);
  }
}
