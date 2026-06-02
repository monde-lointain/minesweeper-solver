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

/* gcd of two positive int64 (Euclidean; native % is one hardware div). */
static int64_t i64_gcd(int64_t a, int64_t b) {
  while (b != 0) {
    int64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/* *acc <- lcm(*acc, d), both > 0. Returns false on int64 overflow (the
 * component then bails to the naive fallback). */
static bool i64_lcm_acc(int64_t* acc, int64_t d) {
  int64_t g = i64_gcd(*acc, d);
  RatI128 prod = i128_mul_i64(*acc / g, d); /* (acc/g)*d == lcm(acc,d), exact */
  if (!i128_fits_i64(prod)) {
    return false;
  }
  *acc = i128_to_i64(prod);
  return true;
}

/* Scale each pivot row r to integers over a per-row common denominator den[r]
 * (= lcm of its rhs + free-var coefficient denominators): fills den/iacc/icoef
 * and the pos/neg coefficient suffix sums (over free-var order), used by the
 * int64 DFS for interval pruning. Then range-checks a conservative per-row
 * magnitude bound into int64 so the DFS needs no per-node overflow guard.
 * Returns false (-> naive fallback) on any int64 overflow, exactly as the
 * rational path bails on rat_invalid(). */
static bool reduce_prep_enum(struct SolverScratch* s) {
  int nv = s->ec.nv;
  int nf = s->rd.nfree;
  for (int r = 0; r < s->rd.rank; ++r) {
    /* den[r] = lcm of the denominators of rhs and each free-var coefficient.
     * (den[r]==1 is the common {0,1}-integer-row case: every scale below is a
     * multiply by 1, which cannot overflow.) */
    int64_t den = s->rd.mat[r][nv].den; /* normalized: den > 0 */
    for (int d = 0; d < nf; ++d) {
      if (!i64_lcm_acc(&den, s->rd.mat[r][s->rd.freevar[d]].den)) {
        return false;
      }
    }
    s->rd.den[r] = den;
    /* iacc[r] = rhs scaled to /den (den is a multiple of rhs.den). */
    struct Rat rhs = s->rd.mat[r][nv];
    RatI128 iacc = i128_mul_i64(rhs.num, den / rhs.den);
    if (!i128_fits_i64(iacc)) {
      return false;
    }
    s->rd.iacc[r] = i128_to_i64(iacc);
    /* free-var coefficients scaled to /den, plus pos/neg suffix sums (built in
     * 128-bit and range-checked, so each stored partial fits int64). */
    s->rd.iposSuf[r][nf] = 0;
    s->rd.inegSuf[r][nf] = 0;
    RatI128 psuf = i128_from_i64(0);
    RatI128 nsuf = i128_from_i64(0);
    for (int d = nf - 1; d >= 0; --d) {
      struct Rat c = s->rd.mat[r][s->rd.freevar[d]];
      RatI128 ic = i128_mul_i64(c.num, den / c.den);
      if (!i128_fits_i64(ic)) {
        return false;
      }
      s->rd.icoef[r][d] = i128_to_i64(ic);
      if (s->rd.icoef[r][d] > 0) {
        psuf = i128_add(psuf, ic);
      } else {
        nsuf = i128_add(nsuf, ic); /* coef<=0; +0 when zero, harmless */
      }
      if (!i128_fits_i64(psuf) || !i128_fits_i64(nsuf)) {
        return false;
      }
      s->rd.iposSuf[r][d] = i128_to_i64(psuf);
      s->rd.inegSuf[r][d] = i128_to_i64(nsuf);
    }
    /* Hoisted overflow guard: every DFS intermediate for this row -- iacc, and
     * the prune values (iacc - posSuf) / (iacc - negSuf) compared against 0 and
     * den -- is bounded in magnitude by |iacc| + 2*(P - N) + den, with
     * P = iposSuf[r][0] >= 0 and N = inegSuf[r][0] <= 0. If that fits int64,
     * the int64 DFS cannot overflow. */
    RatI128 pp = i128_from_i64(s->rd.iposSuf[r][0]);
    RatI128 nn = i128_from_i64(s->rd.inegSuf[r][0]);
    RatI128 diff = i128_sub(pp, nn); /* P - N >= 0 */
    RatI128 bound =
        i128_add(i128_abs(i128_from_i64(s->rd.iacc[r])),
                 i128_add(i128_add(diff, diff), i128_from_i64(s->rd.den[r])));
    if (!i128_fits_i64(bound)) {
      return false;
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
  s->ec.sol[total] += 1.0;
  for (int lv = 0; lv < s->ec.nv; ++lv) {
    if (s->rd.xfull[lv] == 1) {
      s->ec.mine[lv][total] += 1.0;
    }
  }
}

/* DFS over free variable position d in {0,1}, maintaining the integer residual
 * iacc[r] (pivot-row partial value, scaled by den[r]) and pruning rows whose
 * leading var can no longer land in {0,1}. At the leaf, each pivot row's
 * leading var = iacc[r]/den[r] must be exactly 0 or 1 (i.e. iacc[r] == 0 or ==
 * den[r]). All arithmetic is int64; reduce_prep_enum's per-row magnitude bound
 * guarantees none of it overflows, so there is no per-node guard. */
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
      if (s->rd.iacc[r] == 0) {
        s->rd.xfull[s->rd.pivcol[r]] = 0;
      } else if (s->rd.iacc[r] == s->rd.den[r]) {
        s->rd.xfull[s->rd.pivcol[r]] = 1;
      } else {
        return; /* leading var not in {0,1}: not a solution */
      }
    }
    reduced_accumulate(s);
    return;
  }
  for (int val = 0; val <= 1; ++val) {
    s->rd.xfull[s->rd.freevar[d]] = val;
    if (val == 1) {
      for (int r = 0; r < s->rd.rank; ++r) {
        s->rd.iacc[r] -= s->rd.icoef[r][d]; /* coef==0 -> no-op */
      }
    }
    bool ok = true;
    for (int r = 0; r < s->rd.rank && ok; ++r) {
      int64_t den = s->rd.den[r];
      int64_t lo = s->rd.iacc[r] - s->rd.iposSuf[r][d + 1];
      int64_t hi = s->rd.iacc[r] - s->rd.inegSuf[r][d + 1];
      bool c0 = lo <= 0 && hi >= 0;
      bool c1 = lo <= den && hi >= den;
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
        s->rd.iacc[r] += s->rd.icoef[r][d];
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
