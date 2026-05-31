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
 * Fields are grouped by pipeline role, not packed by alignment: the ~35 padding
 * bytes are irrelevant in a ~2.5 MB struct, and grouping aids maintenance.
 */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct SolverScratch {
  /* constraint / variable model */
  int var_of_cell[MAXCELL]; /* cell idx -> var id, or -1 */
  int cell_of_var[MAXCELL]; /* var id -> cell idx */
  int nvar;
  int vstate[MAXCELL];     /* per var: VAR_UNKNOWN/SAFE/MINE */
  int con_var[MAXCELL][8]; /* constraint -> var ids of covered neighbors */
  int con_nv[MAXCELL];
  int con_need[MAXCELL]; /* total mines among those vars (= adjacent) */
  int ncon;

  /* union-find over vars */
  int parent[MAXCELL];

  /* component layout */
  int comp_of_var[MAXCELL]; /* unknown var -> component id, else -1 */
  int local_of_var[MAXCELL];
  int label[MAXCELL]; /* uf root -> component id (build_components scratch) */

  /* per-component normalized results */
  int comp_nv[MAXCOMP];
  int comp_gv[MAXCOMP][CAP_VARS]; /* local -> global var */
  long double comp_shat[MAXCOMP][CAP_VARS + 1];
  long double comp_mhat[MAXCOMP][CAP_VARS][CAP_VARS + 1];
  bool comp_fallback[MAXCOMP];
  long double comp_fb_p[MAXCOMP][CAP_VARS]; /* naive prob if fallback */

  /* DP scratch */
  long double prefix[MAXCOMP + 1][MAXFLEN];
  int prefix_len[MAXCOMP + 1];
  long double suffix[MAXCOMP + 1][MAXFLEN];
  int suffix_len[MAXCOMP + 1];

  /* enumeration scratch (one component at a time) */
  int ec_nv;
  int ec_ncon;
  int ec_con_lv[MAXCELL][8];
  int ec_con_nlv[MAXCELL];
  int ec_con_need[MAXCELL];
  int ec_con_sum[MAXCELL];
  int ec_con_un[MAXCELL]; /* unassigned vars remaining in the constraint */
  int ec_varcon[CAP_VARS][MAXVARCON];
  int ec_varcon_n[CAP_VARS];
  int ec_assign[CAP_VARS];
  long double ec_s[CAP_VARS + 1];
  long double ec_m[CAP_VARS][CAP_VARS + 1];
  int ec_nodes;
  bool ec_overflow;

  /* analyze-local scratch (promoted from function-local statics) */
  int exact_idx[MAXCOMP];
  long double oc[MAXFLEN];
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
  while (s->parent[x] != x) {
    s->parent[x] = s->parent[s->parent[x]];
    x = s->parent[x];
  }
  return x;
}

static void uf_union(struct SolverScratch* s, int a, int b) {
  int ra = uf_find(s, a);
  int rb = uf_find(s, b);
  if (ra != rb) {
    s->parent[ra] = rb;
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
  for (int j = 0; j < s->con_nv[ci]; ++j) {
    int st = s->vstate[s->con_var[ci][j]];
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
  s->nvar = 0;
  s->ncon = 0;
  for (int i = 0; i < b->width * b->height; ++i) {
    s->var_of_cell[i] = -1;
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
          if (s->var_of_cell[idx] < 0) {
            s->var_of_cell[idx] = s->nvar;
            s->cell_of_var[s->nvar] = idx;
            s->vstate[s->nvar] = VAR_UNKNOWN;
            ++s->nvar;
          }
          vars[nv++] = s->var_of_cell[idx];
        }
      }
      if (nv == 0) {
        continue;
      }
      for (int j = 0; j < nv; ++j) {
        s->con_var[s->ncon][j] = vars[j];
      }
      s->con_nv[s->ncon] = nv;
      s->con_need[s->ncon] = c->adjacent;
      ++s->ncon;
    }
  }
}

/* ---- step 2: single-point deduction fixpoint ------------------------------
 */
static void deduce(struct SolverScratch* s) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (int ci = 0; ci < s->ncon; ++ci) {
      int fixed_mines = 0;
      int unknown = 0;
      constraint_tally(s, ci, &fixed_mines, &unknown);
      if (unknown == 0) {
        continue;
      }
      int rem = s->con_need[ci] - fixed_mines;
      int set_to = -1;
      if (rem <= 0) {
        set_to = VAR_SAFE;
      } else if (rem >= unknown) {
        set_to = VAR_MINE;
      }
      if (set_to >= 0) {
        for (int j = 0; j < s->con_nv[ci]; ++j) {
          int v = s->con_var[ci][j];
          if (s->vstate[v] == VAR_UNKNOWN) {
            s->vstate[v] = set_to;
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
  for (int v = 0; v < s->nvar; ++v) {
    s->parent[v] = v;
  }
  for (int ci = 0; ci < s->ncon; ++ci) {
    int first = -1;
    for (int j = 0; j < s->con_nv[ci]; ++j) {
      int v = s->con_var[ci][j];
      if (s->vstate[v] != VAR_UNKNOWN) {
        continue;
      }
      if (first < 0) {
        first = v;
      } else {
        uf_union(s, first, v);
      }
    }
  }
  for (int v = 0; v < s->nvar; ++v) {
    s->label[v] = -1;
    s->comp_of_var[v] = -1;
  }
  int ncomp = 0;
  for (int v = 0; v < s->nvar; ++v) {
    if (s->vstate[v] != VAR_UNKNOWN) {
      continue;
    }
    int r = uf_find(s, v);
    if (s->label[r] < 0) {
      s->label[r] = ncomp++;
    }
    s->comp_of_var[v] = s->label[r];
  }
  return ncomp;
}

/* ---- step 4: per-component enumeration ------------------------------------
 */
static void enum_dfs(struct SolverScratch* s, int i, int total) {
  if (s->ec_overflow) {
    return;
  }
  if (++s->ec_nodes > NODE_BUDGET) {
    s->ec_overflow = true;
    return;
  }
  if (i == s->ec_nv) {
    s->ec_s[total] += 1.0L;
    for (int lv = 0; lv < s->ec_nv; ++lv) {
      if (s->ec_assign[lv] == 1) {
        s->ec_m[lv][total] += 1.0L;
      }
    }
    return;
  }
  for (int val = 0; val <= 1; ++val) {
    bool ok = true;
    for (int t = 0; t < s->ec_varcon_n[i]; ++t) {
      int cj = s->ec_varcon[i][t];
      s->ec_con_sum[cj] += val;
      s->ec_con_un[cj] -= 1;
      bool over = s->ec_con_sum[cj] > s->ec_con_need[cj];
      bool closed_wrong =
          s->ec_con_un[cj] == 0 && s->ec_con_sum[cj] != s->ec_con_need[cj];
      bool cannot_reach =
          s->ec_con_sum[cj] + s->ec_con_un[cj] < s->ec_con_need[cj];
      if (over || closed_wrong || cannot_reach) {
        ok = false;
      }
    }
    if (ok) {
      s->ec_assign[i] = val;
      enum_dfs(s, i + 1, total + val);
    }
    for (int t = 0; t < s->ec_varcon_n[i]; ++t) {
      int cj = s->ec_varcon[i][t];
      s->ec_con_sum[cj] -= val;
      s->ec_con_un[cj] += 1;
    }
  }
}

/* Build the local model for component `comp` and enumerate it. Returns false on
 * fallback (over cap / budget), true on exact success. */
static bool enumerate_component(struct SolverScratch* s, int comp) {
  /* gather local vars */
  s->ec_nv = 0;
  for (int v = 0; v < s->nvar; ++v) {
    if (s->comp_of_var[v] == comp) {
      if (s->ec_nv >= CAP_VARS) {
        return false; /* too many vars: fallback */
      }
      s->local_of_var[v] = s->ec_nv;
      s->comp_gv[comp][s->ec_nv] = v;
      ++s->ec_nv;
    }
  }
  /* gather local constraints */
  s->ec_ncon = 0;
  for (int lv = 0; lv < s->ec_nv; ++lv) {
    s->ec_varcon_n[lv] = 0;
  }
  for (int ci = 0; ci < s->ncon; ++ci) {
    int first_unknown = -1;
    for (int j = 0; j < s->con_nv[ci]; ++j) {
      int v = s->con_var[ci][j];
      if (s->vstate[v] == VAR_UNKNOWN) {
        first_unknown = v;
        break;
      }
    }
    if (first_unknown < 0 || s->comp_of_var[first_unknown] != comp) {
      continue;
    }
    int fixed_mines = 0;
    int nlv = 0;
    for (int j = 0; j < s->con_nv[ci]; ++j) {
      int v = s->con_var[ci][j];
      if (s->vstate[v] == VAR_MINE) {
        ++fixed_mines;
      } else if (s->vstate[v] == VAR_UNKNOWN) {
        int lv = s->local_of_var[v];
        s->ec_con_lv[s->ec_ncon][nlv] = lv;
        s->ec_varcon[lv][s->ec_varcon_n[lv]++] = s->ec_ncon;
        ++nlv;
      }
    }
    s->ec_con_nlv[s->ec_ncon] = nlv;
    s->ec_con_need[s->ec_ncon] = s->con_need[ci] - fixed_mines;
    ++s->ec_ncon;
  }
  /* init enumeration state */
  for (int k = 0; k <= s->ec_nv; ++k) {
    s->ec_s[k] = 0.0L;
  }
  for (int lv = 0; lv < s->ec_nv; ++lv) {
    for (int k = 0; k <= s->ec_nv; ++k) {
      s->ec_m[lv][k] = 0.0L;
    }
  }
  for (int cj = 0; cj < s->ec_ncon; ++cj) {
    s->ec_con_sum[cj] = 0;
    s->ec_con_un[cj] = s->ec_con_nlv[cj];
  }
  s->ec_nodes = 0;
  s->ec_overflow = false;
  enum_dfs(s, 0, 0);
  if (s->ec_overflow) {
    return false;
  }
  /* normalize into comp tables */
  long double total = 0.0L;
  for (int k = 0; k <= s->ec_nv; ++k) {
    total += s->ec_s[k];
  }
  if (total <= 0.0L) {
    return false; /* infeasible component (shouldn't happen): fallback */
  }
  s->comp_nv[comp] = s->ec_nv;
  s->comp_fallback[comp] = false;
  for (int k = 0; k <= s->ec_nv; ++k) {
    s->comp_shat[comp][k] = s->ec_s[k] / total;
  }
  for (int lv = 0; lv < s->ec_nv; ++lv) {
    for (int k = 0; k <= s->ec_nv; ++k) {
      s->comp_mhat[comp][lv][k] = s->ec_m[lv][k] / total;
    }
  }
  return true;
}

/* Naive fallback probabilities for an over-budget component. */
static void fallback_component(struct SolverScratch* s, int comp) {
  s->comp_fallback[comp] = true;
  /* comp_nv / comp_gv already filled up to the cap; recount fully */
  int nv = 0;
  for (int v = 0; v < s->nvar; ++v) {
    if (s->comp_of_var[v] == comp) {
      if (nv < CAP_VARS) {
        s->comp_gv[comp][nv] = v;
      }
      ++nv;
    }
  }
  int use = nv < CAP_VARS ? nv : CAP_VARS;
  s->comp_nv[comp] = use;
  for (int lv = 0; lv < use; ++lv) {
    int v = s->comp_gv[comp][lv];
    int cell = s->cell_of_var[v];
    long double sum = 0.0L;
    int cnt = 0;
    for (int ci = 0; ci < s->ncon; ++ci) {
      int unknown = 0;
      int fixed_mines = 0;
      constraint_tally(s, ci, &fixed_mines, &unknown);
      bool has = false;
      for (int j = 0; j < s->con_nv[ci]; ++j) {
        if (s->cell_of_var[s->con_var[ci][j]] == cell) {
          has = true;
          break;
        }
      }
      if (has && unknown > 0) {
        int rem = s->con_need[ci] - fixed_mines;
        sum += solver_clamp01((long double)rem / (long double)unknown);
        ++cnt;
      }
    }
    s->comp_fb_p[comp][lv] = cnt > 0 ? sum / (long double)cnt : 0.5L;
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
  for (int v = 0; v < s->nvar; ++v) {
    if (s->vstate[v] == VAR_MINE) {
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
    if (!b->cells[i].revealed && s->var_of_cell[i] < 0) {
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
    if (!s->comp_fallback[c]) {
      exact_unknown += s->comp_nv[c];
    } else {
      for (int lv = 0; lv < s->comp_nv[c]; ++lv) {
        *fallback_expected += s->comp_fb_p[c][lv];
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
      if (!s->comp_fallback[c]) {
        s->exact_idx[nexact++] = c;
      }
    }
  }
  ctx->nexact = nexact;

  long double zsum = 0.0L;
  long double interior_num = 0.0L;
  if (ctx->exact_ok) {
    s->prefix[0][0] = 1.0L;
    s->prefix_len[0] = 1;
    for (int e = 0; e < nexact; ++e) {
      int c = s->exact_idx[e];
      conv(s->prefix[e], s->prefix_len[e], s->comp_shat[c], s->comp_nv[c] + 1,
           s->prefix[e + 1], &s->prefix_len[e + 1]);
    }
    s->suffix[nexact][0] = 1.0L;
    s->suffix_len[nexact] = 1;
    for (int e = nexact - 1; e >= 0; --e) {
      int c = s->exact_idx[e];
      conv(s->comp_shat[c], s->comp_nv[c] + 1, s->suffix[e + 1],
           s->suffix_len[e + 1], s->suffix[e], &s->suffix_len[e]);
    }
    /* zsum = sum_F Wall[F] * C(interior_n, r_eff - F) */
    for (int f = 0; f < s->prefix_len[nexact]; ++f) {
      long double w = s->prefix[nexact][f];
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
    int v = s->var_of_cell[i];
    if (v < 0) {
      out->cells[i].is_frontier = false;
      out->cells[i].mine_prob = (double)ctx->interior_prob;
      continue;
    }
    out->cells[i].is_frontier = true;
    if (s->vstate[v] == VAR_SAFE) {
      out->cells[i].mine_prob = 0.0;
    } else if (s->vstate[v] == VAR_MINE) {
      out->cells[i].mine_prob = 1.0;
    } else {
      out->cells[i].mine_prob = 0.5; /* overwritten below if exact */
    }
  }

  /* exact component marginals: P(v) = (sum_k mhat[v][k]*A_c[k]) / zsum */
  if (ctx->exact_ok && ctx->zsum > 0.0L) {
    for (int e = 0; e < ctx->nexact; ++e) {
      int c = s->exact_idx[e];
      /* O_c = prefix[e] (x) suffix[e+1] */
      int oclen = 0;
      conv(s->prefix[e], s->prefix_len[e], s->suffix[e + 1],
           s->suffix_len[e + 1], s->oc, &oclen);
      /* A_c[k] = sum_t oc[t] * C(interior_n, r_eff - k - t) */
      long double ac[CAP_VARS + 1];
      for (int k = 0; k <= s->comp_nv[c]; ++k) {
        long double sm = 0.0L;
        for (int t = 0; t < oclen; ++t) {
          if (s->oc[t] == 0.0L) {
            continue;
          }
          sm += s->oc[t] * binom_ld(ctx->interior_n, ctx->r_eff - k - t);
        }
        ac[k] = sm;
      }
      for (int lv = 0; lv < s->comp_nv[c]; ++lv) {
        long double num = 0.0L;
        for (int k = 0; k <= s->comp_nv[c]; ++k) {
          num += s->comp_mhat[c][lv][k] * ac[k];
        }
        int cell = s->cell_of_var[s->comp_gv[c][lv]];
        out->cells[cell].mine_prob = (double)solver_clamp01(num / ctx->zsum);
      }
    }
  }

  /* fallback components: naive probs directly */
  for (int c = 0; c < ctx->ncomp; ++c) {
    if (!ctx->exact_ok || s->comp_fallback[c]) {
      for (int lv = 0; lv < s->comp_nv[c]; ++lv) {
        int cell = s->cell_of_var[s->comp_gv[c][lv]];
        out->cells[cell].mine_prob = (double)s->comp_fb_p[c][lv];
      }
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
