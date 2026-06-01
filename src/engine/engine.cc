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

#include "engine_scratch.h" /* sizing enum + scratch structs + enumerate_reduced */
#include "solver/geom.h"    /* struct Pt, solver_neighbor */
#include "solver/util.h"

enum { VAR_UNKNOWN = -1, VAR_SAFE = 0, VAR_MINE = 1 };

static const long double EPS = 1e-9L;
static const int NODE_BUDGET = 5000000;

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

void solver_test_set_force_reduce(struct SolverScratch* s, bool on) {
  s->force_reduce = on;
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
      struct Pt cell = {x, y};
      for (int k = 0; k < 8; ++k) {
        struct Pt nb;
        if (!solver_neighbor(b, cell, k, &nb)) {
          continue;
        }
        if (!cell_covered(b, nb.x, nb.y)) {
          continue;
        }
        int idx = game_index(b, nb.x, nb.y);
        if (s->cm.var_of_cell[idx] < 0) {
          s->cm.var_of_cell[idx] = s->cm.nvar;
          s->cm.cell_of_var[s->cm.nvar] = idx;
          s->cm.vstate[s->cm.nvar] = VAR_UNKNOWN;
          ++s->cm.nvar;
        }
        vars[nv++] = s->cm.var_of_cell[idx];
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

/* Build the ec local constraint model for component `comp` (vars + local
 * indices precomputed by layout_components). If pin_var >= 0 that var is
 * excluded (pinned safe, contributing no mine) — the info-gain re-solve. Writes
 * the ec-local -> component-local map into ec2orig[0..m) when ec2orig != NULL;
 * zeroes ec.sol / ec.mine and inits con_sum / con_un. Touches ONLY ec scratch
 * (never s->res / s->cm / s->comp), so an info-gain re-solve leaves the stored
 * marginals intact. Returns m (#ec vars; == res.nv[comp] when pin_var < 0), 0
 * if the pin removes the only var, or -1 if pinning makes a constraint
 * infeasible. */
static int build_local_model(struct SolverScratch* s, int comp, int pin_var,
                             int* ec2orig) {
  int nv_full = s->res.nv[comp];
  int pin_local = (pin_var >= 0) ? s->comp.local_of_var[pin_var] : -1;
  int ec_of_orig[MAX_COMP_VARS]; /* original local -> ec local (-1 for pin) */
  int m = 0;
  for (int ol = 0; ol < nv_full; ++ol) {
    if (ol == pin_local) {
      ec_of_orig[ol] = -1;
      continue;
    }
    ec_of_orig[ol] = m;
    if (ec2orig != NULL) {
      ec2orig[m] = ol;
    }
    ++m;
  }
  if (m == 0) {
    return 0; /* pin removed the component's only var */
  }
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
      } else if (s->cm.vstate[v] == VAR_UNKNOWN) {
        int lv = ec_of_orig[s->comp.local_of_var[v]];
        if (lv < 0) {
          continue; /* pinned-safe candidate: no mine, not a var */
        }
        s->ec.con_lv[s->ec.ncon][nlv] = lv;
        s->ec.varcon[lv][s->ec.varcon_n[lv]++] = s->ec.ncon;
        ++nlv;
      }
    }
    int need = s->cm.con_need[ci] - fixed_mines; /* pin (=0) adds no mines */
    if (nlv == 0) {
      /* Unreachable when pin_var < 0 (first_unknown is always counted), so this
       * is byte-identical to the old enumerate_component for that path. */
      if (need != 0) {
        return -1; /* pinning the candidate safe is infeasible here */
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
  return m;
}

/* Enumerate the prepared ec model (m vars) directly or via Gaussian reduction.
 * allow_force lets the test-only force_reduce flag drive the reduced path on
 * small systems (production-inert). Returns the total solution count, or 0 on
 * overflow / infeasible (the caller treats <= 0 as a fallback). */
static long double enumerate_local(struct SolverScratch* s, int m,
                                   bool allow_force) {
  if (m > CAP_VARS || (allow_force && s->force_reduce)) {
    if (!enumerate_reduced(s)) { /* resets nodes/overflow itself */
      return 0.0L;
    }
  } else {
    s->ec.nodes = 0;
    s->ec.overflow = false;
    enum_dfs(s, 0, 0); /* direct: branch all vars */
    if (s->ec.overflow) {
      return 0.0L;
    }
  }
  long double total = 0.0L;
  for (int k = 0; k <= m; ++k) {
    total += s->ec.sol[k];
  }
  return total;
}

/* Build the local model for component `comp` and enumerate it exactly. Returns
 * false on fallback (over the direct cap / node budget / infeasible), true on
 * exact success (and then reserves this component's compact shat/mhat slots,
 * advancing the running offsets). */
static bool enumerate_component(struct SolverScratch* s, int comp,
                                int* shat_used, int* mhat_used) {
  int nv = s->res.nv[comp];
  if (nv > MAX_COMP_VARS) {
    return false; /* too big to store exactly: fallback */
  }
  if (build_local_model(s, comp, -1, NULL) != nv) {
    return false; /* infeasible (cannot happen with no pin): fallback */
  }
  long double total = enumerate_local(s, nv, true);
  if (total <= 0.0L) {
    return false; /* node-budget overflow or infeasible: fallback */
  }
  /* normalize into compact comp tables */
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
/* A polynomial as a non-owning (coefficients, length) view — bundles the
 * (buffer, len) data clump that conv and the combine DP thread together. POD;
 * storage stays in the caller's flat scratch arrays. */
struct Poly {
  long double* v;
  int len;
};

static inline struct Poly poly(long double* v, int len) {
  struct Poly p = {v, len};
  return p;
}

/* out-buffer convolution a (x) b -> {out, a.len + b.len - 1}. Same coefficient
 * accumulation order as before (golden-pinned). */
static struct Poly conv(struct Poly a, struct Poly b, long double* out) {
  int n = a.len + b.len - 1;
  for (int i = 0; i < n; ++i) {
    out[i] = 0.0L;
  }
  for (int i = 0; i < a.len; ++i) {
    if (a.v[i] == 0.0L) {
      continue;
    }
    for (int j = 0; j < b.len; ++j) {
      out[i + j] += a.v[i] * b.v[j];
    }
  }
  return poly(out, n);
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
  out->best.x = 0;
  out->best.y = 0;
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
      struct Poly r =
          conv(poly(s->dp.fbdist, s->dp.fbdist_len), poly(bern, 2), s->dp.wall);
      for (int i = 0; i < r.len; ++i) {
        s->dp.fbdist[i] = r.v[i];
      }
      s->dp.fbdist_len = r.len;
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
      s->dp.prefix_len[e + 1] =
          conv(poly(s->dp.prefix[e], s->dp.prefix_len[e]),
               poly(res_shat(s, c), s->res.nv[c] + 1), s->dp.prefix[e + 1])
              .len;
    }
    s->dp.suffix[nexact][0] = 1.0L;
    s->dp.suffix_len[nexact] = 1;
    for (int e = nexact - 1; e >= 0; --e) {
      int c = s->dp.exact_idx[e];
      s->dp.suffix_len[e] =
          conv(poly(res_shat(s, c), s->res.nv[c] + 1),
               poly(s->dp.suffix[e + 1], s->dp.suffix_len[e + 1]),
               s->dp.suffix[e])
              .len;
    }
    /* Wall = (exact prefix) (x) fbdist = full frontier-mine distribution. */
    struct Poly wall =
        conv(poly(s->dp.prefix[nexact], s->dp.prefix_len[nexact]),
             poly(s->dp.fbdist, s->dp.fbdist_len), s->dp.wall);
    /* zsum = sum_F Wall[F] * C(interior_n, r_eff - F) */
    for (int f = 0; f < wall.len; ++f) {
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

/* deduced/single-point + interior baseline for every covered cell.
 * out->cells was already zeroed by memset in solver_analyze. */
static void write_baseline_probs(const struct Board* b, struct Analysis* out,
                                 struct SolverScratch* s,
                                 const struct AnalyzeCtx* ctx) {
  int ncells = ctx->ncells;
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
}

/* exact component marginals: P(v) = (sum_k mhat[v][k]*A_c[k]) / zsum */
static void write_exact_marginals(struct Analysis* out, struct SolverScratch* s,
                                  const struct AnalyzeCtx* ctx) {
  if (!(ctx->exact_ok && ctx->zsum > 0.0L)) {
    return;
  }
  for (int e = 0; e < ctx->nexact; ++e) {
    int c = s->dp.exact_idx[e];
    /* O_c = prefix[e] (x) suffix[e+1] */
    struct Poly oc =
        conv(poly(s->dp.prefix[e], s->dp.prefix_len[e]),
             poly(s->dp.suffix[e + 1], s->dp.suffix_len[e + 1]), s->dp.oc);
    /* fold in the fallback distribution so c's complement covers ALL other
     * frontier mines (other exact comps + fallback comps). */
    struct Poly oc2 = conv(oc, poly(s->dp.fbdist, s->dp.fbdist_len), s->dp.oc2);
    int oc2len = oc2.len;
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
static void write_fallback_probs(struct Analysis* out, struct SolverScratch* s,
                                 const struct AnalyzeCtx* ctx) {
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
static void clamp_approx_probs(const struct Board* b, struct Analysis* out,
                               struct SolverScratch* s,
                               const struct AnalyzeCtx* ctx) {
  const double kApproxLo = 1e-6; /* >> EPS (1e-9): never reads as forced */
  int ncells = ctx->ncells;
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

/* Write every covered cell's P(mine): deduced/interior baseline, exact
 * component marginals, naive fallback components, then the approximate-source
 * 0/1 clamp. out->cells was already zeroed by memset in solver_analyze. */
static void write_cell_probs(const struct Board* b, struct Analysis* out,
                             struct SolverScratch* s,
                             const struct AnalyzeCtx* ctx) {
  write_baseline_probs(b, out, s, ctx);
  write_exact_marginals(out, s, ctx);
  write_fallback_probs(out, s, ctx);
  clamp_approx_probs(b, out, s, ctx);
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
  out->best.x = best_x;
  out->best.y = best_y;
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
  out->best.x = -1;
  out->best.y = -1;
  out->exact = true; /* default: terminal/start/safe have no estimation */

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

  /* exact iff the DP ran exactly AND no component used the naive fallback. */
  bool any_fb = false;
  for (int c = 0; c < ctx.ncomp; ++c) {
    if (s->res.fallback[c]) {
      any_fb = true;
      break;
    }
  }
  out->exact = ctx.exact_ok && !any_fb;
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
  int ec2orig[MAX_COMP_VARS]; /* ec local -> original local */
  /* Re-build the component's local model with the candidate pinned safe.
   * allow_force=true keeps the reduced path differentially testable; it is
   * production-inert (force_reduce is always false in production). */
  int m = build_local_model(s, comp, pin_var, ec2orig);
  if (m <= 0) {
    return 0; /* pin removed the only var (m==0) or is infeasible (m<0) */
  }
  long double total = enumerate_local(s, m, true);
  if (total <= 0.0L) {
    return 0; /* node-budget overflow or infeasible */
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
 * best_prob. Must stay >= solver_recommend_move's band (RECOMMEND_BAND) so
 * every cell the policy considers is covered; widening it only costs time,
 * never correctness.
 */
static const double INFOGAIN_BAND = 0.05;

/* Fill info_gain for non-forced frontier cells near the minimum risk in an
 * exact component. Called only at EVAL_GUESS; clobbers only the transient ec/rd
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
