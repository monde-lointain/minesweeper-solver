/* engine.cc — exact Minesweeper probability engine (Stream B).
 *
 * Pipeline (flags ignored throughout; every covered cell is an unknown):
 *   1. Build constraints from revealed numbers over their covered neighbors.
 *   2. Single-point deduction to a fixpoint (shrinks the CSP, marks forced cells).
 *   3. Split the residual unknown frontier into connected components.
 *   4. Backtrack-enumerate each component: per mine-count k, #solutions and
 *      per-variable mine incidence.
 *   5. Combine components + interior cells under the global mine count via a
 *      leave-one-out convolution DP -> exact per-cell P(mine).
 * Exact enumeration subsumes subset reduction for correctness (a var that is a
 * mine in every solution gets P=1 -> forced_mine, etc.); subset reduction would
 * only shrink components, which the per-component cap + node budget already bound.
 *
 * Orthodox C++: POD, plain enums, pointers, C headers, fixed file-scope scratch
 * (single-threaded; analysis runs once per move).
 */
#include "solver/engine.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum { VAR_UNKNOWN = -1, VAR_SAFE = 0, VAR_MINE = 1 };

enum {
  MAXCELL = BOARD_MAX_CELLS,
  CAP_VARS = 24,    /* per-component enumeration cap */
  MAXCOMP = 128,    /* exact-DP component cap */
  MAXFLEN = 256,    /* exact-DP total-frontier-mine cap + 1 */
  MAXVARCON = 8     /* a covered cell borders <= 8 numbered cells */
};

static const long double EPS = 1e-9L;
static const int NODE_BUDGET = 5000000;

/* ---- constraint / variable model ------------------------------------------ */
static int g_var_of_cell[MAXCELL]; /* cell idx -> var id, or -1 */
static int g_cell_of_var[MAXCELL]; /* var id -> cell idx */
static int g_nvar;
static int g_vstate[MAXCELL]; /* per var: VAR_UNKNOWN/SAFE/MINE */

static int g_con_var[MAXCELL][8]; /* constraint -> var ids of covered neighbors */
static int g_con_nv[MAXCELL];
static int g_con_need[MAXCELL]; /* total mines among those vars (= adjacent) */
static int g_ncon;

/* union-find over vars */
static int g_parent[MAXCELL];

/* component layout */
static int g_comp_of_var[MAXCELL]; /* unknown var -> component id, else -1 */
static int g_local_of_var[MAXCELL];

/* per-component normalized results */
static int g_comp_nv[MAXCOMP];
static int g_comp_gv[MAXCOMP][CAP_VARS];          /* local -> global var */
static long double g_comp_shat[MAXCOMP][CAP_VARS + 1];
static long double g_comp_mhat[MAXCOMP][CAP_VARS][CAP_VARS + 1];
static bool g_comp_fallback[MAXCOMP];
static long double g_comp_fb_p[MAXCOMP][CAP_VARS]; /* naive prob if fallback */

/* DP scratch */
static long double g_prefix[MAXCOMP + 1][MAXFLEN];
static int g_prefix_len[MAXCOMP + 1];
static long double g_suffix[MAXCOMP + 1][MAXFLEN];
static int g_suffix_len[MAXCOMP + 1];

/* enumeration scratch (one component at a time) */
static int g_ec_nv;
static int g_ec_ncon;
static int g_ec_con_lv[MAXCELL][8];
static int g_ec_con_nlv[MAXCELL];
static int g_ec_con_need[MAXCELL];
static int g_ec_con_sum[MAXCELL];
static int g_ec_con_un[MAXCELL]; /* unassigned vars remaining in the constraint */
static int g_ec_varcon[CAP_VARS][MAXVARCON];
static int g_ec_varcon_n[CAP_VARS];
static int g_ec_assign[CAP_VARS];
static long double g_ec_s[CAP_VARS + 1];
static long double g_ec_m[CAP_VARS][CAP_VARS + 1];
static int g_ec_nodes;
static bool g_ec_overflow;

/* ---- helpers -------------------------------------------------------------- */
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

static long double clamp01(long double v) {
  if (v < 0.0L) {
    return 0.0L;
  }
  if (v > 1.0L) {
    return 1.0L;
  }
  return v;
}

static int uf_find(int x) {
  while (g_parent[x] != x) {
    g_parent[x] = g_parent[g_parent[x]];
    x = g_parent[x];
  }
  return x;
}

static void uf_union(int a, int b) {
  int ra = uf_find(a);
  int rb = uf_find(b);
  if (ra != rb) {
    g_parent[ra] = rb;
  }
}

static bool cell_covered(const struct Board *b, int x, int y) {
  return !b->cells[game_index(b, x, y)].revealed;
}

/* ---- step 1: build constraints + variables -------------------------------- */
static void build_constraints(const struct Board *b) {
  g_nvar = 0;
  g_ncon = 0;
  for (int i = 0; i < b->width * b->height; ++i) {
    g_var_of_cell[i] = -1;
  }
  for (int y = 0; y < b->height; ++y) {
    for (int x = 0; x < b->width; ++x) {
      const struct Cell *c = &b->cells[game_index(b, x, y)];
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
          if (g_var_of_cell[idx] < 0) {
            g_var_of_cell[idx] = g_nvar;
            g_cell_of_var[g_nvar] = idx;
            g_vstate[g_nvar] = VAR_UNKNOWN;
            ++g_nvar;
          }
          vars[nv++] = g_var_of_cell[idx];
        }
      }
      if (nv == 0) {
        continue;
      }
      for (int j = 0; j < nv; ++j) {
        g_con_var[g_ncon][j] = vars[j];
      }
      g_con_nv[g_ncon] = nv;
      g_con_need[g_ncon] = c->adjacent;
      ++g_ncon;
    }
  }
}

/* ---- step 2: single-point deduction fixpoint ------------------------------ */
static void deduce(void) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (int ci = 0; ci < g_ncon; ++ci) {
      int fixed_mines = 0;
      int unknown = 0;
      for (int j = 0; j < g_con_nv[ci]; ++j) {
        int st = g_vstate[g_con_var[ci][j]];
        if (st == VAR_MINE) {
          ++fixed_mines;
        } else if (st == VAR_UNKNOWN) {
          ++unknown;
        }
      }
      if (unknown == 0) {
        continue;
      }
      int rem = g_con_need[ci] - fixed_mines;
      int set_to = -1;
      if (rem <= 0) {
        set_to = VAR_SAFE;
      } else if (rem >= unknown) {
        set_to = VAR_MINE;
      }
      if (set_to >= 0) {
        for (int j = 0; j < g_con_nv[ci]; ++j) {
          int v = g_con_var[ci][j];
          if (g_vstate[v] == VAR_UNKNOWN) {
            g_vstate[v] = set_to;
            changed = true;
          }
        }
      }
    }
  }
}

/* ---- step 3: components over residual unknown vars ------------------------ */
static int build_components(void) {
  for (int v = 0; v < g_nvar; ++v) {
    g_parent[v] = v;
  }
  for (int ci = 0; ci < g_ncon; ++ci) {
    int first = -1;
    for (int j = 0; j < g_con_nv[ci]; ++j) {
      int v = g_con_var[ci][j];
      if (g_vstate[v] != VAR_UNKNOWN) {
        continue;
      }
      if (first < 0) {
        first = v;
      } else {
        uf_union(first, v);
      }
    }
  }
  static int label[MAXCELL];
  for (int v = 0; v < g_nvar; ++v) {
    label[v] = -1;
    g_comp_of_var[v] = -1;
  }
  int ncomp = 0;
  for (int v = 0; v < g_nvar; ++v) {
    if (g_vstate[v] != VAR_UNKNOWN) {
      continue;
    }
    int r = uf_find(v);
    if (label[r] < 0) {
      label[r] = ncomp++;
    }
    g_comp_of_var[v] = label[r];
  }
  return ncomp;
}

/* ---- step 4: per-component enumeration ------------------------------------ */
static void enum_dfs(int i, int total) {
  if (g_ec_overflow) {
    return;
  }
  if (++g_ec_nodes > NODE_BUDGET) {
    g_ec_overflow = true;
    return;
  }
  if (i == g_ec_nv) {
    g_ec_s[total] += 1.0L;
    for (int lv = 0; lv < g_ec_nv; ++lv) {
      if (g_ec_assign[lv] == 1) {
        g_ec_m[lv][total] += 1.0L;
      }
    }
    return;
  }
  for (int val = 0; val <= 1; ++val) {
    bool ok = true;
    for (int t = 0; t < g_ec_varcon_n[i]; ++t) {
      int cj = g_ec_varcon[i][t];
      g_ec_con_sum[cj] += val;
      g_ec_con_un[cj] -= 1;
      bool over = g_ec_con_sum[cj] > g_ec_con_need[cj];
      bool closed_wrong =
          g_ec_con_un[cj] == 0 && g_ec_con_sum[cj] != g_ec_con_need[cj];
      bool cannot_reach =
          g_ec_con_sum[cj] + g_ec_con_un[cj] < g_ec_con_need[cj];
      if (over || closed_wrong || cannot_reach) {
        ok = false;
      }
    }
    if (ok) {
      g_ec_assign[i] = val;
      enum_dfs(i + 1, total + val);
    }
    for (int t = 0; t < g_ec_varcon_n[i]; ++t) {
      int cj = g_ec_varcon[i][t];
      g_ec_con_sum[cj] -= val;
      g_ec_con_un[cj] += 1;
    }
  }
}

/* Build the local model for component `comp` and enumerate it. Returns false on
 * fallback (over cap / budget), true on exact success. */
static bool enumerate_component(int comp) {
  /* gather local vars */
  g_ec_nv = 0;
  for (int v = 0; v < g_nvar; ++v) {
    if (g_comp_of_var[v] == comp) {
      if (g_ec_nv >= CAP_VARS) {
        return false; /* too many vars: fallback */
      }
      g_local_of_var[v] = g_ec_nv;
      g_comp_gv[comp][g_ec_nv] = v;
      ++g_ec_nv;
    }
  }
  /* gather local constraints */
  g_ec_ncon = 0;
  for (int lv = 0; lv < g_ec_nv; ++lv) {
    g_ec_varcon_n[lv] = 0;
  }
  for (int ci = 0; ci < g_ncon; ++ci) {
    int first_unknown = -1;
    for (int j = 0; j < g_con_nv[ci]; ++j) {
      int v = g_con_var[ci][j];
      if (g_vstate[v] == VAR_UNKNOWN) {
        first_unknown = v;
        break;
      }
    }
    if (first_unknown < 0 || g_comp_of_var[first_unknown] != comp) {
      continue;
    }
    int fixed_mines = 0;
    int nlv = 0;
    for (int j = 0; j < g_con_nv[ci]; ++j) {
      int v = g_con_var[ci][j];
      if (g_vstate[v] == VAR_MINE) {
        ++fixed_mines;
      } else if (g_vstate[v] == VAR_UNKNOWN) {
        int lv = g_local_of_var[v];
        g_ec_con_lv[g_ec_ncon][nlv] = lv;
        g_ec_varcon[lv][g_ec_varcon_n[lv]++] = g_ec_ncon;
        ++nlv;
      }
    }
    g_ec_con_nlv[g_ec_ncon] = nlv;
    g_ec_con_need[g_ec_ncon] = g_con_need[ci] - fixed_mines;
    ++g_ec_ncon;
  }
  /* init enumeration state */
  for (int k = 0; k <= g_ec_nv; ++k) {
    g_ec_s[k] = 0.0L;
  }
  for (int lv = 0; lv < g_ec_nv; ++lv) {
    for (int k = 0; k <= g_ec_nv; ++k) {
      g_ec_m[lv][k] = 0.0L;
    }
  }
  for (int cj = 0; cj < g_ec_ncon; ++cj) {
    g_ec_con_sum[cj] = 0;
    g_ec_con_un[cj] = g_ec_con_nlv[cj];
  }
  g_ec_nodes = 0;
  g_ec_overflow = false;
  enum_dfs(0, 0);
  if (g_ec_overflow) {
    return false;
  }
  /* normalize into comp tables */
  long double total = 0.0L;
  for (int k = 0; k <= g_ec_nv; ++k) {
    total += g_ec_s[k];
  }
  if (total <= 0.0L) {
    return false; /* infeasible component (shouldn't happen): fallback */
  }
  g_comp_nv[comp] = g_ec_nv;
  g_comp_fallback[comp] = false;
  for (int k = 0; k <= g_ec_nv; ++k) {
    g_comp_shat[comp][k] = g_ec_s[k] / total;
  }
  for (int lv = 0; lv < g_ec_nv; ++lv) {
    for (int k = 0; k <= g_ec_nv; ++k) {
      g_comp_mhat[comp][lv][k] = g_ec_m[lv][k] / total;
    }
  }
  return true;
}

/* Naive fallback probabilities for an over-budget component. */
static void fallback_component(int comp) {
  g_comp_fallback[comp] = true;
  /* g_comp_nv / g_comp_gv already filled up to the cap; recount fully */
  int nv = 0;
  for (int v = 0; v < g_nvar; ++v) {
    if (g_comp_of_var[v] == comp) {
      if (nv < CAP_VARS) {
        g_comp_gv[comp][nv] = v;
      }
      ++nv;
    }
  }
  int use = nv < CAP_VARS ? nv : CAP_VARS;
  g_comp_nv[comp] = use;
  for (int lv = 0; lv < use; ++lv) {
    int v = g_comp_gv[comp][lv];
    int cell = g_cell_of_var[v];
    long double sum = 0.0L;
    int cnt = 0;
    for (int ci = 0; ci < g_ncon; ++ci) {
      bool has = false;
      int unknown = 0;
      int fixed_mines = 0;
      for (int j = 0; j < g_con_nv[ci]; ++j) {
        int vv = g_con_var[ci][j];
        if (g_cell_of_var[vv] == cell) {
          has = true;
        }
        if (g_vstate[vv] == VAR_UNKNOWN) {
          ++unknown;
        } else if (g_vstate[vv] == VAR_MINE) {
          ++fixed_mines;
        }
      }
      if (has && unknown > 0) {
        int rem = g_con_need[ci] - fixed_mines;
        sum += clamp01((long double)rem / (long double)unknown);
        ++cnt;
      }
    }
    g_comp_fb_p[comp][lv] = cnt > 0 ? sum / (long double)cnt : 0.5L;
  }
}

/* ---- step 5: global combination ------------------------------------------- */
static void conv(const long double *a, int la, const long double *b, int lb,
                 long double *out, int *lout) {
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

/* ---- public entry --------------------------------------------------------- */
void solver_analyze(const struct Board *b, struct Analysis *out) {
  memset(out, 0, sizeof *out);
  out->best_x = -1;
  out->best_y = -1;

  if (b->status == GAME_WON) {
    out->eval = EVAL_SOLVED;
    return;
  }
  if (b->status == GAME_LOST) {
    out->eval = EVAL_LOST;
    return;
  }

  int ncells = b->width * b->height;

  /* EVAL_START: nothing revealed yet -> uniform, suggest a corner. */
  if (b->revealed_count == 0) {
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
    out->best_is_interior = true;
    out->interior_prob = (double)uniform;
    out->interior_count = ncells;
    out->eval = EVAL_START;
    return;
  }

  build_constraints(b);
  deduce();

  int known_mines = 0;
  for (int v = 0; v < g_nvar; ++v) {
    if (g_vstate[v] == VAR_MINE) {
      ++known_mines;
    }
  }

  int ncomp = build_components();
  bool exact_ok = (ncomp <= MAXCOMP);

  /* interior cells: covered, not a frontier variable. */
  int interior_count = 0;
  for (int i = 0; i < ncells; ++i) {
    if (!b->cells[i].revealed && g_var_of_cell[i] < 0) {
      ++interior_count;
    }
  }

  /* enumerate components */
  long double fallback_expected = 0.0L;
  if (exact_ok) {
    for (int c = 0; c < ncomp; ++c) {
      if (!enumerate_component(c)) {
        fallback_component(c);
      }
    }
    /* total unknown frontier vars across EXACT components must fit MAXFLEN. */
    int exact_unknown = 0;
    for (int c = 0; c < ncomp; ++c) {
      if (!g_comp_fallback[c]) {
        exact_unknown += g_comp_nv[c];
      } else {
        for (int lv = 0; lv < g_comp_nv[c]; ++lv) {
          fallback_expected += g_comp_fb_p[c][lv];
        }
      }
    }
    if (exact_unknown >= MAXFLEN) {
      exact_ok = false;
    }
  }

  int interior_n = interior_count;
  int rem_mines = b->mines - known_mines;
  /* remove fallback components' expected mines from the budget the exact DP +
   * interior share. */
  int r_eff = rem_mines - (int)llroundl(fallback_expected);
  r_eff = (r_eff < 0) ? 0 : r_eff;

  /* Build prefix/suffix convolutions over EXACT components only. */
  static int exact_idx[MAXCOMP];
  int nexact = 0;
  if (exact_ok) {
    for (int c = 0; c < ncomp; ++c) {
      if (!g_comp_fallback[c]) {
        exact_idx[nexact++] = c;
      }
    }
  }

  long double zsum = 0.0L;
  long double interior_num = 0.0L;
  if (exact_ok) {
    g_prefix[0][0] = 1.0L;
    g_prefix_len[0] = 1;
    for (int e = 0; e < nexact; ++e) {
      int c = exact_idx[e];
      conv(g_prefix[e], g_prefix_len[e], g_comp_shat[c], g_comp_nv[c] + 1,
           g_prefix[e + 1], &g_prefix_len[e + 1]);
    }
    g_suffix[nexact][0] = 1.0L;
    g_suffix_len[nexact] = 1;
    for (int e = nexact - 1; e >= 0; --e) {
      int c = exact_idx[e];
      conv(g_comp_shat[c], g_comp_nv[c] + 1, g_suffix[e + 1], g_suffix_len[e + 1],
           g_suffix[e], &g_suffix_len[e]);
    }
    /* zsum = sum_F Wall[F] * C(interior_n, r_eff - F) */
    for (int f = 0; f < g_prefix_len[nexact]; ++f) {
      long double w = g_prefix[nexact][f];
      if (w == 0.0L) {
        continue;
      }
      long double bcoef = binom_ld(interior_n, r_eff - f);
      zsum += w * bcoef;
      interior_num += w * bcoef * (long double)(r_eff - f);
    }
  }

  long double interior_prob = 0.0L;
  if (exact_ok && zsum > 0.0L && interior_n > 0) {
    interior_prob = interior_num / (zsum * (long double)interior_n);
  } else if (interior_n > 0) {
    /* fallback: uniform remaining density */
    interior_prob = clamp01((long double)r_eff / (long double)interior_n);
  }

  /* ---- write per-cell probabilities ------------------------------------- */
  for (int i = 0; i < ncells; ++i) {
    out->cells[i].mine_prob = 0.0;
    out->cells[i].is_frontier = false;
    out->cells[i].forced_safe = false;
    out->cells[i].forced_mine = false;
  }

  /* deduced + interior */
  for (int i = 0; i < ncells; ++i) {
    if (b->cells[i].revealed) {
      continue;
    }
    int v = g_var_of_cell[i];
    if (v < 0) {
      out->cells[i].is_frontier = false;
      out->cells[i].mine_prob = (double)interior_prob;
      continue;
    }
    out->cells[i].is_frontier = true;
    if (g_vstate[v] == VAR_SAFE) {
      out->cells[i].mine_prob = 0.0;
    } else if (g_vstate[v] == VAR_MINE) {
      out->cells[i].mine_prob = 1.0;
    } else {
      out->cells[i].mine_prob = 0.5; /* overwritten below if exact */
    }
  }

  /* exact component marginals: P(v) = (sum_k mhat[v][k]*A_c[k]) / zsum */
  if (exact_ok && zsum > 0.0L) {
    for (int e = 0; e < nexact; ++e) {
      int c = exact_idx[e];
      /* O_c = prefix[e] (x) suffix[e+1] */
      static long double oc[MAXFLEN];
      int oclen = 0;
      conv(g_prefix[e], g_prefix_len[e], g_suffix[e + 1], g_suffix_len[e + 1], oc,
           &oclen);
      /* A_c[k] = sum_t oc[t] * C(interior_n, r_eff - k - t) */
      long double ac[CAP_VARS + 1];
      for (int k = 0; k <= g_comp_nv[c]; ++k) {
        long double s = 0.0L;
        for (int t = 0; t < oclen; ++t) {
          if (oc[t] == 0.0L) {
            continue;
          }
          s += oc[t] * binom_ld(interior_n, r_eff - k - t);
        }
        ac[k] = s;
      }
      for (int lv = 0; lv < g_comp_nv[c]; ++lv) {
        long double num = 0.0L;
        for (int k = 0; k <= g_comp_nv[c]; ++k) {
          num += g_comp_mhat[c][lv][k] * ac[k];
        }
        int cell = g_cell_of_var[g_comp_gv[c][lv]];
        out->cells[cell].mine_prob = (double)clamp01(num / zsum);
      }
    }
  }

  /* fallback components: naive probs directly */
  for (int c = 0; c < ncomp; ++c) {
    if (!exact_ok || g_comp_fallback[c]) {
      for (int lv = 0; lv < g_comp_nv[c]; ++lv) {
        int cell = g_cell_of_var[g_comp_gv[c][lv]];
        out->cells[cell].mine_prob = (double)g_comp_fb_p[c][lv];
      }
    }
  }

  /* forced flags + best move (row-major first on ties) */
  double best = 2.0;
  int best_x = -1;
  int best_y = -1;
  bool best_interior = false;
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
        best_interior = !out->cells[i].is_frontier;
      }
    }
  }

  out->interior_count = interior_n;
  out->interior_prob = (double)interior_prob;
  out->best_x = best_x;
  out->best_y = best_y;
  out->best_prob = best_x >= 0 ? best : 0.0;
  out->best_is_interior = best_interior;
  if (best_x < 0) {
    out->eval = EVAL_SOLVED;
  } else if ((long double)best < EPS) {
    out->eval = EVAL_SAFE;
  } else {
    out->eval = EVAL_GUESS;
  }
}
