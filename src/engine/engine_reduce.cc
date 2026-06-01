/* engine_reduce.cc — exact-rational Gaussian-elimination reduction path for the
 * probability engine (extracted from engine.cc, behavior-identical).
 *
 * Row-reduces a component's {0,1} constraint system to RREF, then enumerates
 * only the free variables (back-substituting leading vars and pruning when they
 * leave {0,1}). All arithmetic is exact rational; any overflow / infeasible row
 * / too-many-free / node-budget condition bails to the caller's naive fallback,
 * so a wrong marginal is never emitted. Output (ec.sol / ec.mine) matches the
 * direct path so the combine DP is untouched. Touches only s->rd and s->ec.
 *
 * Orthodox C++: POD, plain functions, pointers, C headers.
 */
#include "engine_scratch.h"

/* Separate, smaller budget for the reduced free-var DFS: structured frontiers
 * finish far under it; high-entropy ones bail fast to fallback (each reduced
 * node costs O(rank) rational ops, so a huge budget would be too slow). */
static const int REDUCED_NODE_BUDGET = 30000;

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
bool enumerate_reduced(struct SolverScratch* s) {
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
