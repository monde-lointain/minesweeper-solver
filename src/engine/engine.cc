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

#include "solver/util.h"

enum { VAR_UNKNOWN = -1, VAR_SAFE = 0, VAR_MINE = 1 };

enum {
  MAXCELL = BOARD_MAX_CELLS,
  CAP_VARS = 24, /* per-component enumeration cap */
  MAXCOMP = 128, /* exact-DP component cap */
  MAXFLEN = 256, /* exact-DP total-frontier-mine cap + 1 */
  MAXVARCON = 8  /* a covered cell borders <= 8 numbered cells */
};

static const long double EPS = 1e-9L;
static const int NODE_BUDGET = 5000000;

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

/* per-component normalized enumeration results */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct CompResults {
  int nv[MAXCOMP];
  int gv[MAXCOMP][CAP_VARS]; /* local -> global var */
  long double shat[MAXCOMP][CAP_VARS + 1];
  long double mhat[MAXCOMP][CAP_VARS][CAP_VARS + 1];
  bool fallback[MAXCOMP];
  long double fb_p[MAXCOMP][CAP_VARS]; /* naive prob if fallback */
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
  int varcon[CAP_VARS][MAXVARCON];
  int varcon_n[CAP_VARS];
  int assign[CAP_VARS];
  long double sol[CAP_VARS + 1];            /* per mine-count k: #solutions */
  long double mine[CAP_VARS][CAP_VARS + 1]; /* per (var,k): mine incidence */
  int nodes;
  bool overflow;
};

/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct SolverScratch {
  struct ConstraintModel cm;
  struct CompLayout comp;
  struct CompResults res;
  struct CombineDP dp;
  struct EnumScratch ec;
};

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

/* Build the local model for component `comp` and enumerate it. Returns false on
 * fallback (over cap / budget), true on exact success. */
static bool enumerate_component(struct SolverScratch* s, int comp) {
  /* gather local vars */
  s->ec.nv = 0;
  for (int v = 0; v < s->cm.nvar; ++v) {
    if (s->comp.of_var[v] == comp) {
      if (s->ec.nv >= CAP_VARS) {
        return false; /* too many vars: fallback */
      }
      s->comp.local_of_var[v] = s->ec.nv;
      s->res.gv[comp][s->ec.nv] = v;
      ++s->ec.nv;
    }
  }
  /* gather local constraints */
  s->ec.ncon = 0;
  for (int lv = 0; lv < s->ec.nv; ++lv) {
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
  for (int k = 0; k <= s->ec.nv; ++k) {
    s->ec.sol[k] = 0.0L;
  }
  for (int lv = 0; lv < s->ec.nv; ++lv) {
    for (int k = 0; k <= s->ec.nv; ++k) {
      s->ec.mine[lv][k] = 0.0L;
    }
  }
  for (int cj = 0; cj < s->ec.ncon; ++cj) {
    s->ec.con_sum[cj] = 0;
    s->ec.con_un[cj] = s->ec.con_nlv[cj];
  }
  s->ec.nodes = 0;
  s->ec.overflow = false;
  enum_dfs(s, 0, 0);
  if (s->ec.overflow) {
    return false;
  }
  /* normalize into comp tables */
  long double total = 0.0L;
  for (int k = 0; k <= s->ec.nv; ++k) {
    total += s->ec.sol[k];
  }
  if (total <= 0.0L) {
    return false; /* infeasible component (shouldn't happen): fallback */
  }
  s->res.nv[comp] = s->ec.nv;
  s->res.fallback[comp] = false;
  for (int k = 0; k <= s->ec.nv; ++k) {
    s->res.shat[comp][k] = s->ec.sol[k] / total;
  }
  for (int lv = 0; lv < s->ec.nv; ++lv) {
    for (int k = 0; k <= s->ec.nv; ++k) {
      s->res.mhat[comp][lv][k] = s->ec.mine[lv][k] / total;
    }
  }
  return true;
}

/* Naive fallback probabilities for an over-budget component. */
static void fallback_component(struct SolverScratch* s, int comp) {
  s->res.fallback[comp] = true;
  /* comp_nv / comp_gv already filled up to the cap; recount fully */
  int nv = 0;
  for (int v = 0; v < s->cm.nvar; ++v) {
    if (s->comp.of_var[v] == comp) {
      if (nv < CAP_VARS) {
        s->res.gv[comp][nv] = v;
      }
      ++nv;
    }
  }
  int use = nv < CAP_VARS ? nv : CAP_VARS;
  s->res.nv[comp] = use;
  for (int lv = 0; lv < use; ++lv) {
    int v = s->res.gv[comp][lv];
    int cell = s->cm.cell_of_var[v];
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
    s->res.fb_p[comp][lv] = cnt > 0 ? sum / (long double)cnt : 0.5L;
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
  for (int c = 0; c < ncomp; ++c) {
    if (!enumerate_component(s, c)) {
      fallback_component(s, c);
    }
  }
  /* total unknown frontier vars across EXACT components must fit MAXFLEN. */
  int exact_unknown = 0;
  for (int c = 0; c < ncomp; ++c) {
    if (!s->res.fallback[c]) {
      exact_unknown += s->res.nv[c];
    } else {
      for (int lv = 0; lv < s->res.nv[c]; ++lv) {
        *fallback_expected += s->res.fb_p[c][lv];
      }
    }
  }
  if (exact_unknown >= MAXFLEN) {
    *exact_ok = false;
  }
}

/* Combine exact components + interior under the global mine count:
 * prefix/suffix convolution DP -> ctx->{r_eff, nexact, zsum, interior_prob}. */
static void compute_interior_prob(const struct Board* b,
                                  struct SolverScratch* s,
                                  struct AnalyzeCtx* ctx,
                                  long double fallback_expected) {
  int rem_mines = b->mines - ctx->known_mines;
  /* remove fallback components' expected mines from the budget the exact DP +
   * interior share. */
  int r_eff = rem_mines - (int)llroundl(fallback_expected);
  r_eff = (r_eff < 0) ? 0 : r_eff;
  ctx->r_eff = r_eff;

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
    s->dp.prefix[0][0] = 1.0L;
    s->dp.prefix_len[0] = 1;
    for (int e = 0; e < nexact; ++e) {
      int c = s->dp.exact_idx[e];
      conv(s->dp.prefix[e], s->dp.prefix_len[e], s->res.shat[c],
           s->res.nv[c] + 1, s->dp.prefix[e + 1], &s->dp.prefix_len[e + 1]);
    }
    s->dp.suffix[nexact][0] = 1.0L;
    s->dp.suffix_len[nexact] = 1;
    for (int e = nexact - 1; e >= 0; --e) {
      int c = s->dp.exact_idx[e];
      conv(s->res.shat[c], s->res.nv[c] + 1, s->dp.suffix[e + 1],
           s->dp.suffix_len[e + 1], s->dp.suffix[e], &s->dp.suffix_len[e]);
    }
    /* zsum = sum_F Wall[F] * C(interior_n, r_eff - F) */
    for (int f = 0; f < s->dp.prefix_len[nexact]; ++f) {
      long double w = s->dp.prefix[nexact][f];
      if (w == 0.0L) {
        continue;
      }
      long double bcoef = binom_ld(ctx->interior_n, r_eff - f);
      zsum += w * bcoef;
      interior_num += w * bcoef * (long double)(r_eff - f);
    }
  }
  ctx->zsum = zsum;

  long double interior_prob = 0.0L;
  if (ctx->exact_ok && zsum > 0.0L && ctx->interior_n > 0) {
    interior_prob = interior_num / (zsum * (long double)ctx->interior_n);
  } else if (ctx->interior_n > 0) {
    /* fallback: uniform remaining density */
    interior_prob =
        solver_clamp01((long double)r_eff / (long double)ctx->interior_n);
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
      /* A_c[k] = sum_t oc[t] * C(interior_n, r_eff - k - t) */
      long double ac[CAP_VARS + 1];
      for (int k = 0; k <= s->res.nv[c]; ++k) {
        long double sm = 0.0L;
        for (int t = 0; t < oclen; ++t) {
          if (s->dp.oc[t] == 0.0L) {
            continue;
          }
          sm += s->dp.oc[t] * binom_ld(ctx->interior_n, ctx->r_eff - k - t);
        }
        ac[k] = sm;
      }
      for (int lv = 0; lv < s->res.nv[c]; ++lv) {
        long double num = 0.0L;
        for (int k = 0; k <= s->res.nv[c]; ++k) {
          num += s->res.mhat[c][lv][k] * ac[k];
        }
        int cell = s->cm.cell_of_var[s->res.gv[c][lv]];
        out->cells[cell].mine_prob = (double)solver_clamp01(num / ctx->zsum);
      }
    }
  }

  /* fallback components: naive probs directly */
  for (int c = 0; c < ctx->ncomp; ++c) {
    if (!ctx->exact_ok || s->res.fallback[c]) {
      for (int lv = 0; lv < s->res.nv[c]; ++lv) {
        int cell = s->cm.cell_of_var[s->res.gv[c][lv]];
        out->cells[cell].mine_prob = (double)s->res.fb_p[c][lv];
      }
    }
  }

  /* Approximate paths must never masquerade as proofs. When any component fell
   * back (or the exact DP overflowed), the global mine budget is a rounded
   * point estimate (see compute_interior_prob's r_eff), so a derived P(mine) of
   * exactly 0 or 1 is NOT proven — it is an artifact of collapsing a
   * distribution. Clamp every non-deduced covered cell away from {0,1} so
   * pick_best_move cannot mark it forced_safe/forced_mine and the policy cannot
   * "safely" walk into a mine. Single-point-deduced cells (VAR_SAFE/VAR_MINE)
   * are real proofs and keep their exact 0/1. */
  bool approx = !ctx->exact_ok;
  for (int c = 0; c < ctx->ncomp && !approx; ++c) {
    if (s->res.fallback[c]) {
      approx = true;
    }
  }
  if (approx) {
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
      double p = out->cells[i].mine_prob;
      if (p < kApproxLo) {
        p = kApproxLo;
      } else if (p > 1.0 - kApproxLo) {
        p = 1.0 - kApproxLo;
      }
      out->cells[i].mine_prob = p;
    }
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
