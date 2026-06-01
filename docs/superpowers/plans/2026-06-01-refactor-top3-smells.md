# Top-3 Smell Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the three most pungent smells from the smell audit — long multi-responsibility functions (Bloater), the `struct Move`/`struct Pt` duplicate type + middle-man (Dispensable/Coupler), and the `(coeffs,len)` polynomial data-clump (Bloater) — without changing any observable behavior.

**Architecture:** Three independent, behavior-preserving refactors. (1) *Collapse Move into Pt* across the bench. (2) *Extract Function* on `engine.cc::write_cell_probs`. (3) *Introduce Parameter Object* `struct Poly` for `conv` + the combine DP. Plus the two GUI Bloaters (`app_init`, `app_iterate`) via *Extract Function*. All stay Orthodox C++ (POD structs, free functions, pointers, C headers).

**Tech Stack:** C++17 (Orthodox subset), CMake + Ninja/Make, GoogleTest via `ctest`, clang-22 + clang-tidy (Orthodoxy plugin), clang-format.

---

## Refactor discipline (read first)

These are **behavior-preserving refactors, not features** — standard TDD ("write a failing test first") does **not** apply. The discipline here is **characterization-test-guarded refactoring**:

1. **Establish green baseline** — run the guarding test(s), confirm PASS *before* touching code.
2. **Refactor** (copy → compile new path → switch caller → delete old, per CLAUDE.md).
3. **Confirm still green** — same test(s) must PASS unchanged.
4. **Format + tidy + commit.**

If a guarding test changes value, the refactor changed behavior — **stop and revert**, do not regenerate the golden baseline.

**Guarding tests per refactor:**
- **Move→Pt:** compiler (type-checked) + `bench_policy_test`, `bench_policy_infogain_test`, `bench_runner_test`, `bench_test`, `recommend_test`.
- **engine.cc (extract + Poly):** `Golden.MatchesBaseline` (primary), plus `engine_test`, `engine_exact_test`, `engine_infogain_test`, `engine_reduction_test`, `recommend_test`, `reasoning_test`.
- **app.cc (`app_init`/`app_iterate`):** ⚠️ **no unit coverage.** Guard = `cmake --build` succeeds + `make tidy` clean + `make run` visual smoke (window opens, overlay box + companion panel render, F9/F10 toggle).

**Standard commands** (run from repo root `/home/clydew372/development/projects/cpp/minesweeper-solver`):
- Build: `cmake --build build -j"$(nproc)"`
- Targeted tests: `ctest --test-dir build -R '<regex>' --output-on-failure`
- Full suite: `make test`
- Format: `make format` (source of truth for whitespace — never hand-fight it)
- Lint: `make tidy`

**Ordering constraint:** Phase 2 (extract) and Phase 3 (Poly) both edit `engine.cc` — **do Phase 2 before Phase 3** (Phase 3 edits the function Phase 2 extracts). Phase 1 (bench) and Phase 4 (app) are independent and may run in any order / in parallel worktrees.

---

## File Structure

| File | Phase | Responsibility after refactor |
|------|-------|-------------------------------|
| `src/bench/policy.h` | 1 | `struct Move` removed; `policy_select` takes `struct Pt*`; includes `solver/geom.h` |
| `src/bench/policy.cc` | 1 | baseline branch writes `*out = a->best;` |
| `src/bench/policy_infogain.h/.cc` | 1 | signatures use `struct Pt*`; body is a pass-through to `solver_recommend_move` |
| `src/bench/runner.cc` | 1 | `struct Pt mv;` |
| `tests/bench_policy_test.cc`, `tests/bench_policy_infogain_test.cc` | 1 | `struct Pt mv;` |
| `src/engine/engine.cc` | 2 | `write_cell_probs` becomes a 4-call orchestrator over new `write_baseline_probs` / `write_exact_marginals` / `write_fallback_probs` / `clamp_approx_probs` |
| `src/engine/engine.cc` | 3 | `struct Poly` + `poly()` view; `conv` returns `Poly`; 7 call sites bundle (ptr,len) |
| `src/app/app.cc` | 4 | `app_init` extracts `app_init_imgui` + `app_init_panel`; `app_iterate` extracts `app_resolve_face` + `app_draw_panel` |

---

## Phase 1 — Collapse `struct Move` into `struct Pt`

**Smell:** Alternative Classes with Different Interfaces + Middle Man. `struct Move {int x; int y;}` is structurally identical to `struct Pt`; `policy_infogain_select` exists only to copy `Pt`→`Move`.

**Files:**
- Modify: `src/bench/policy.h`, `src/bench/policy.cc`, `src/bench/policy_infogain.h`, `src/bench/policy_infogain.cc`, `src/bench/runner.cc`
- Test: `tests/bench_policy_test.cc`, `tests/bench_policy_infogain_test.cc`

- [ ] **Step 1: Establish green baseline**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build -R 'BenchPolicy|PolicyInfogain|BenchRunner|Bench\.' --output-on-failure`
Expected: PASS (all bench policy/runner tests green before any edit).

- [ ] **Step 2: Remove `struct Move`, switch `policy.h` to `struct Pt`**

In `src/bench/policy.h`, replace the include block + `struct Move` definition. Change:

```c
#include "minesweeper/game.h"  /* struct Board, game_index */
#include "solver/engine.h"     /* struct Analysis */

/* A covered cell to reveal. */
struct Move {
  int x;
  int y;
};

enum PolicyId {
```

to:

```c
#include "minesweeper/game.h"  /* struct Board, game_index */
#include "solver/engine.h"     /* struct Analysis */
#include "solver/geom.h"       /* struct Pt — the covered-cell coordinate */

enum PolicyId {
```

Then change the `policy_select` declaration signature from `struct Move* out` to `struct Pt* out`:

```c
/* Choose a covered cell to reveal for the given analysis. Returns 0 and writes
 * *out on success; returns -1 if no covered cell exists. Pure: reads b and a,
 * writes only *out. */
int policy_select(int policy_id, const struct Board* b,
                  const struct Analysis* a, struct Pt* out);
```

- [ ] **Step 3: Update `policy.cc` to write `struct Pt`**

In `src/bench/policy.cc`, change the `policy_select` signature and the baseline branch. Replace:

```c
int policy_select(int policy_id, const struct Board* b,
                  const struct Analysis* a, struct Move* out) {
  if (policy_id == POLICY_INFOGAIN) {
    return policy_infogain_select(b, a, out);
  }
  /* POLICY_BASELINE: forward the engine's precomputed min-prob pick. */
  (void)b;
  if (a->best.x < 0 || a->best.y < 0) {
    return -1; /* no covered cell (solved/terminal) */
  }
  out->x = a->best.x;
  out->y = a->best.y;
  return 0;
}
```

with:

```c
int policy_select(int policy_id, const struct Board* b,
                  const struct Analysis* a, struct Pt* out) {
  if (policy_id == POLICY_INFOGAIN) {
    return policy_infogain_select(b, a, out);
  }
  /* POLICY_BASELINE: forward the engine's precomputed min-prob pick. */
  (void)b;
  if (a->best.x < 0 || a->best.y < 0) {
    return -1; /* no covered cell (solved/terminal) */
  }
  *out = a->best;
  return 0;
}
```

- [ ] **Step 4: Collapse the `policy_infogain` middle-man**

In `src/bench/policy_infogain.h`, change the comment + signature from `struct Move*` to `struct Pt*`. Replace:

```c
#include "policy.h"           /* struct Move */
```

with:

```c
#include "solver/geom.h"      /* struct Pt */
```

and change the declaration to:

```c
int policy_infogain_select(const struct Board* b, const struct Analysis* a,
                           struct Pt* out);
```

In `src/bench/policy_infogain.cc`, the Pt→Move copy disappears — it becomes a pass-through. Replace:

```c
int policy_infogain_select(const struct Board* b, const struct Analysis* a,
                           struct Move* out) {
  struct Pt p;
  int rc = solver_recommend_move(b, a, &p);
  if (rc == 0) {
    out->x = p.x;
    out->y = p.y;
  }
  return rc;
}
```

with:

```c
int policy_infogain_select(const struct Board* b, const struct Analysis* a,
                           struct Pt* out) {
  return solver_recommend_move(b, a, out);
}
```

- [ ] **Step 5: Update `runner.cc`**

In `src/bench/runner.cc`, the only `struct Move` is the local `mv` (line ~53). `struct Pt` is already visible (via `solver/engine.h` → `solver/geom.h`). Change:

```c
    struct Move mv;
    if (policy_select(cfg->policy_id, &b, a, &mv) != 0) {
```

to:

```c
    struct Pt mv;
    if (policy_select(cfg->policy_id, &b, a, &mv) != 0) {
```

- [ ] **Step 6: Update the two test files**

In `tests/bench_policy_test.cc` (lines ~27, ~51) and `tests/bench_policy_infogain_test.cc` (lines ~75, ~94, ~115, ~132, ~147-148, ~165), replace every `struct Move` declaration with `struct Pt`. Field access (`mv.x`, `mv.y`, `m1.x`, `m2.x`, …) is unchanged because both structs expose identical `x`/`y` members.

Run to find every site: `grep -rn 'struct Move' tests/` → expected: zero matches after edits.

- [ ] **Step 7: Build, test, confirm still green**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build -R 'BenchPolicy|PolicyInfogain|BenchRunner|Bench\.|Recommend' --output-on-failure`
Expected: PASS (identical results to Step 1).

Then confirm the type is fully gone: `grep -rn 'struct Move' src tests include` → expected: zero matches.

- [ ] **Step 8: Format, tidy, commit**

```bash
make format
make tidy
git add -A && git commit -m "refactor(bench): collapse struct Move into struct Pt; drop infogain copy"
```

---

## Phase 2 — Extract Function: `engine.cc::write_cell_probs`

**Smell:** Long Function (~120 lines, four distinct phases). Decompose into four named helpers; `write_cell_probs` becomes an orchestrator. **Pure mechanical extraction — no arithmetic, no ordering changes.**

**Files:**
- Modify: `src/engine/engine.cc:733-852`
- Test (guard): `Golden.MatchesBaseline`, `engine_test`, `engine_exact_test`, `engine_infogain_test`, `recommend_test`

- [ ] **Step 1: Establish green baseline**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build -R 'Golden|Engine|Recommend|Reasoning' --output-on-failure`
Expected: PASS. **Record the exact PASS set** — it must be byte-identical after the extraction.

- [ ] **Step 2: Add the four helpers above `write_cell_probs`**

Insert these four `static` functions immediately *before* the current `write_cell_probs` definition (before line 733, after `compute_interior_prob`). Each body is lifted verbatim from the corresponding block of the current `write_cell_probs` — do not alter any expression or loop order.

```c
/* deduced/single-point + interior baseline for every covered cell. */
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
static void write_exact_marginals(const struct Board* b, struct Analysis* out,
                                  struct SolverScratch* s,
                                  const struct AnalyzeCtx* ctx) {
  if (!(ctx->exact_ok && ctx->zsum > 0.0L)) {
    return;
  }
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
 * from an APPROXIMATE source (naive fallback, or the crude no-DP path) is not a
 * real proof, so clamp it away from {0,1} — else pick_best_move marks it forced
 * and the policy walks into a mispriced mine. PER-COMPONENT: an EXACT
 * component's 0/1 IS a proof and is kept; single-point-deduced and interior
 * cells keep their exact 0/1. */
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
      int comp = s->comp.of_var[v]; /* frontier cell: approximate iff fallback */
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
```

- [ ] **Step 3: Replace `write_cell_probs` body with the orchestrator**

Replace the entire current `write_cell_probs` (lines 733-852) with:

```c
/* Write every covered cell's P(mine): deduced/interior baseline, exact
 * component marginals, naive fallback components, then the approximate-source
 * 0/1 clamp. out->cells was already zeroed by memset in solver_analyze. */
static void write_cell_probs(const struct Board* b, struct Analysis* out,
                             struct SolverScratch* s,
                             const struct AnalyzeCtx* ctx) {
  write_baseline_probs(b, out, s, ctx);
  write_exact_marginals(b, out, s, ctx);
  write_fallback_probs(out, s, ctx);
  clamp_approx_probs(b, out, s, ctx);
}
```

- [ ] **Step 4: Build and confirm the characterization gate is unchanged**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build -R 'Golden|Engine|Recommend|Reasoning' --output-on-failure`
Expected: PASS — identical set to Step 1. If `Golden.MatchesBaseline` fails, the extraction altered behavior; **revert and re-diff**, do NOT run `GOLDEN_CAPTURE=1`.

- [ ] **Step 5: Format, tidy, commit**

```bash
make format
make tidy
git add -A && git commit -m "refactor(engine): extract write_cell_probs into four phase helpers"
```

---

## Phase 3 — Introduce Parameter Object: `struct Poly` for `conv` + combine DP

**Smell:** Data Clump — every polynomial is a `(long double* buffer, int length)` pair threaded through a 6-argument `conv` (incl. an `int* lout` out-param) and the DP. Bundle into a non-owning view `struct Poly`; `conv` returns a `Poly` instead of writing a separate length. Underlying flat storage arrays are unchanged, so **floating-point accumulation order is identical** (the `Golden` gate proves it).

**Files:**
- Modify: `src/engine/engine.cc` (`conv` definition + all 7 call sites: lines ~508, 648, 693, 700, 705, 766, 771)
- Test (guard): `Golden.MatchesBaseline`, `engine_test`, `engine_exact_test`, `engine_infogain_test`, `engine_reduction_test`

- [ ] **Step 1: Establish green baseline**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build -R 'Golden|Engine' --output-on-failure`
Expected: PASS.

- [ ] **Step 2: Define `struct Poly` + `poly()` and rewrite `conv`**

Replace the current `conv` (lines 508-523):

```c
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
```

with the bundled view + return-based form (arithmetic byte-identical):

```c
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
```

- [ ] **Step 3: Update call site in `build_fbdist` (line ~648)**

Replace:

```c
      int outlen = 0;
      conv(s->dp.fbdist, s->dp.fbdist_len, bern, 2, s->dp.wall, &outlen);
      for (int i = 0; i < outlen; ++i) {
        s->dp.fbdist[i] = s->dp.wall[i];
      }
      s->dp.fbdist_len = outlen;
```

with:

```c
      struct Poly r =
          conv(poly(s->dp.fbdist, s->dp.fbdist_len), poly(bern, 2), s->dp.wall);
      for (int i = 0; i < r.len; ++i) {
        s->dp.fbdist[i] = r.v[i];
      }
      s->dp.fbdist_len = r.len;
```

- [ ] **Step 4: Update the prefix/suffix/wall call sites in `compute_interior_prob` (lines ~693, 700, 705)**

Replace the prefix loop body:

```c
    for (int e = 0; e < nexact; ++e) {
      int c = s->dp.exact_idx[e];
      conv(s->dp.prefix[e], s->dp.prefix_len[e], res_shat(s, c),
           s->res.nv[c] + 1, s->dp.prefix[e + 1], &s->dp.prefix_len[e + 1]);
    }
```

with:

```c
    for (int e = 0; e < nexact; ++e) {
      int c = s->dp.exact_idx[e];
      s->dp.prefix_len[e + 1] =
          conv(poly(s->dp.prefix[e], s->dp.prefix_len[e]),
               poly(res_shat(s, c), s->res.nv[c] + 1), s->dp.prefix[e + 1])
              .len;
    }
```

Replace the suffix loop body:

```c
    for (int e = nexact - 1; e >= 0; --e) {
      int c = s->dp.exact_idx[e];
      conv(res_shat(s, c), s->res.nv[c] + 1, s->dp.suffix[e + 1],
           s->dp.suffix_len[e + 1], s->dp.suffix[e], &s->dp.suffix_len[e]);
    }
```

with:

```c
    for (int e = nexact - 1; e >= 0; --e) {
      int c = s->dp.exact_idx[e];
      s->dp.suffix_len[e] =
          conv(poly(res_shat(s, c), s->res.nv[c] + 1),
               poly(s->dp.suffix[e + 1], s->dp.suffix_len[e + 1]),
               s->dp.suffix[e])
              .len;
    }
```

Replace the wall convolution:

```c
    int wall_len = 0;
    conv(s->dp.prefix[nexact], s->dp.prefix_len[nexact], s->dp.fbdist,
         s->dp.fbdist_len, s->dp.wall, &wall_len);
    /* zsum = sum_F Wall[F] * C(interior_n, r_eff - F) */
    for (int f = 0; f < wall_len; ++f) {
```

with:

```c
    struct Poly wall =
        conv(poly(s->dp.prefix[nexact], s->dp.prefix_len[nexact]),
             poly(s->dp.fbdist, s->dp.fbdist_len), s->dp.wall);
    /* zsum = sum_F Wall[F] * C(interior_n, r_eff - F) */
    for (int f = 0; f < wall.len; ++f) {
```

> Note: the loop body below references `s->dp.wall[f]` directly — leave those as-is (`wall.v` aliases `s->dp.wall`). Only the loop bound `f < wall_len` → `f < wall.len` changes.

- [ ] **Step 5: Update the `oc`/`oc2` call sites in `write_exact_marginals` (lines ~766, 771)**

(These now live in the `write_exact_marginals` helper created in Phase 2.) Replace:

```c
    int oclen = 0;
    conv(s->dp.prefix[e], s->dp.prefix_len[e], s->dp.suffix[e + 1],
         s->dp.suffix_len[e + 1], s->dp.oc, &oclen);
    /* fold in the fallback distribution ... */
    int oc2len = 0;
    conv(s->dp.oc, oclen, s->dp.fbdist, s->dp.fbdist_len, s->dp.oc2, &oc2len);
```

with:

```c
    struct Poly oc = conv(poly(s->dp.prefix[e], s->dp.prefix_len[e]),
                          poly(s->dp.suffix[e + 1], s->dp.suffix_len[e + 1]),
                          s->dp.oc);
    /* fold in the fallback distribution ... */
    struct Poly oc2 =
        conv(oc, poly(s->dp.fbdist, s->dp.fbdist_len), s->dp.oc2);
    int oc2len = oc2.len;
```

> `s->dp.oc2[t]` accesses below stay as-is (`oc2.v` aliases `s->dp.oc2`); only the `oc2len` source changes. The local `oclen` is no longer needed — `oc` is consumed immediately by the next `conv`.

- [ ] **Step 6: Build and confirm the golden gate is unchanged**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build -R 'Golden|Engine|Recommend|Reasoning' --output-on-failure`
Expected: PASS — identical to Step 1. A `Golden.MatchesBaseline` failure means FP order drifted; **revert**, do not recapture.

Sanity-check no stale 6-arg calls remain: `grep -n 'conv(' src/engine/engine.cc` → every match is the new 3-arg form.

- [ ] **Step 7: Format, tidy, commit**

```bash
make format
make tidy
git add -A && git commit -m "refactor(engine): bundle (coeffs,len) into struct Poly; conv returns Poly"
```

---

## Phase 4 — Extract Function: `app_init` + `app_iterate` (GUI Bloaters)

**Smell:** Long Function. `app_init` (126 lines) and `app_iterate` (100 lines) interleave the companion-window concern with core init/frame logic. ⚠️ **No unit coverage** — guard is build + tidy + manual smoke.

**Files:**
- Modify: `src/app/app.cc` (`app_init:145-271`, `app_iterate:591-691`)

- [ ] **Step 1: Establish a building, lint-clean baseline**

Run: `cmake --build build -j"$(nproc)" && make tidy`
Expected: build succeeds, `make tidy` reports no new diagnostics. (No tests cover this file.)

- [ ] **Step 2: Extract `app_init_imgui` and `app_init_panel`**

Add these two `static` functions immediately *before* `app_init` (after `app_start_timer`, before the `/* ---- init ---- */` banner's function). Bodies lifted verbatim from `app_init`:

```c
/* Game-window ImGui: context + SDL3/renderer backends + chrome theme. */
static void app_init_imgui(struct AppState* s) {
  IMGUI_CHECKVERSION();
  s->ctx_game = (void*)ImGui::CreateContext();
  ImGuiIO* io = &ImGui::GetIO();
  io->IniFilename = NULL; /* no imgui.ini */
  ImGui_ImplSDL3_InitForSDLRenderer(s->window, s->renderer);
  ImGui_ImplSDLRenderer3_Init(s->renderer);
  ui_apply_theme();
}

/* Companion reasoning window: its own window + renderer + ImGui context (the
 * SDL_Renderer backend has no multi-viewport). vsync OFF (static content) so it
 * never double-stalls the game's vsync. Sets s->panel_on iff fully created. */
static void app_init_panel(struct AppState* s) {
  s->panel_window = SDL_CreateWindow("Solver - Reasoning", 300, 380, 0);
  if (s->panel_window != NULL) {
    /* Parent the companion to the game window so the compositor keeps it
     * stacked above (xdg_toplevel.set_parent on Wayland / WM_TRANSIENT_FOR on
     * X11). Self-raise (SDL_RaiseWindow) is ignored by Wayland compositors
     * without a focus serial, so a parent relationship is the reliable way to
     * keep the companion in front rather than behind the game. */
    SDL_SetWindowParent(s->panel_window, s->window);
    s->panel_renderer = SDL_CreateRenderer(s->panel_window, NULL);
  }
  if (s->panel_renderer != NULL) {
    s->ctx_panel = (void*)ImGui::CreateContext();
    ImGui::SetCurrentContext((ImGuiContext*)s->ctx_panel);
    ImGuiIO* pio = &ImGui::GetIO();
    pio->IniFilename = NULL;
    ImGui_ImplSDL3_InitForSDLRenderer(s->panel_window, s->panel_renderer);
    ImGui_ImplSDLRenderer3_Init(s->panel_renderer);
    ImGui::StyleColorsDark(); /* default ImGui dark, not the game chrome */
    ImGui::SetCurrentContext((ImGuiContext*)s->ctx_game);
    s->panel_on = true;
  }
}
```

Then in `app_init`, replace the inline ImGui block (lines 211-243, from `/* ImGui. */` through the panel-context `s->panel_on = true; }`) with:

```c
  app_init_imgui(s);
  app_init_panel(s);
```

Leave the rest of `app_init` (hover init, `pending_name_level`, `overlay_on`, `app_new_game`, window positioning + show) untouched.

- [ ] **Step 3: Extract `app_resolve_face` and `app_draw_panel`**

Add these two `static` functions immediately *before* `app_iterate` (after `app_apply_actions`). Bodies lifted verbatim from `app_iterate`:

```c
/* Smiley face for non-pressing states (pressing states are set by input). */
static void app_resolve_face(struct AppState* s) {
  if (!s->pressing_face && !s->pressing_board && !s->chord_active) {
    if (s->board.status == GAME_LOST) {
      s->button_face = BTN_LOSE;
    } else if (s->board.status == GAME_WON) {
      s->button_face = BTN_WIN;
    } else {
      s->button_face = BTN_HAPPY;
    }
  }
}

/* Companion reasoning window: separate ImGui context + renderer. No-op when the
 * panel is hidden or was never created. */
static void app_draw_panel(struct AppState* s) {
  if (!(s->panel_on && s->panel_window != NULL && s->ctx_panel != NULL)) {
    return;
  }
  struct ReasoningView rv;
  reasoning_build(&s->board, &s->analysis, s->hover, &rv);

  ImGui::SetCurrentContext((ImGuiContext*)s->ctx_panel);
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  ImGuiIO* pio = &ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(pio->DisplaySize);
  ImGui::Begin("reasoning", NULL,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  reasoning_panel_draw(&rv);
  ImGui::End();

  SDL_SetRenderDrawColor(s->panel_renderer, 30, 30, 30, 255);
  SDL_RenderClear(s->panel_renderer);
  ImGui::Render();
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s->panel_renderer);
  SDL_RenderPresent(s->panel_renderer);

  ImGui::SetCurrentContext((ImGuiContext*)s->ctx_game);
}
```

Then in `app_iterate`: replace the face-resolve block (lines 633-642, `/* Resolve smiley face ... */` + its `if`) with:

```c
  app_resolve_face(s);
```

and replace the entire companion-window block (lines 660-688, `/* Companion reasoning window: ... */` through its closing brace) with:

```c
  app_draw_panel(s);
```

- [ ] **Step 4: Build + lint**

Run: `cmake --build build -j"$(nproc)" && make tidy`
Expected: build succeeds; `make tidy` clean (no new diagnostics vs Step 1).

- [ ] **Step 5: Manual smoke test (no automated coverage)**

Run: `make run`
Verify, then close the window:
- Game window opens; left-click reveals, right-click flags, smiley resets.
- Blue overlay box renders on the recommended cell; green/red proven markers appear; **F10** toggles the overlay.
- Companion "Solver - Reasoning" window renders the verdict/recommendation panel; **F9** toggles it; its close button hides (does not quit).

Expected: identical behavior to before the extraction.

- [ ] **Step 6: Format and commit**

```bash
make format
git add -A && git commit -m "refactor(app): extract imgui/panel init + face/panel draw from app_init/app_iterate"
```

---

## Self-Review

- **Spec coverage:** Top-3 smells each have a phase — Bloater/long-fn (Phases 2 & 4), Move/Pt duplicate + middle-man (Phase 1), `(coeffs,len)` clump (Phase 3). ✔
- **Type consistency:** `policy_select`/`policy_infogain_select` both take `struct Pt* out` post-Phase-1; `conv` is `struct Poly conv(struct Poly, struct Poly, long double*)` and every Phase-3 call site uses `poly(...)` + `.len`. The Phase-2 helper `write_exact_marginals` is the function Phase-3 Step 5 edits — names match. ✔
- **No placeholders:** every code/command step is concrete. ✔
- **Ordering:** Phase 2 precedes Phase 3 (both touch `engine.cc`; 3 edits 2's output). Phases 1 & 4 independent. ✔
- **Behavior preservation:** no arithmetic, branch, or loop-order change in any phase; `Golden.MatchesBaseline` guards the engine phases; compiler + bench tests guard Phase 1. ✔

---

## Unresolved questions

1. **Fully delete the `policy_infogain` seam?** After Phase 1, `policy_infogain_select` is a one-line pass-through to `solver_recommend_move`. Inline it into `policy.cc` and delete `policy_infogain.{h,cc}` + retarget `bench_policy_infogain_test.cc` to `policy_select(POLICY_INFOGAIN, …)`? Kept as a seam here to bound churn — confirm whether the extra test rewrite is wanted.
2. **Push `struct Poly` into the scratch struct?** This plan keeps the parallel `prefix_len[]`/`suffix_len[]`/`fbdist_len` fields (view-only Poly). Converting `CombineDP` to store `struct Poly prefix[MAXCOMP+1]` over renamed storage would kill the *field*-level clump too, at the cost of a struct-layout change (more risk, golden must re-pass). Worth a follow-up, or leave the fields?
3. **How far to decompose `app_iterate`?** This plan extracts the face + panel concerns. The main draw block (clear → `render_frame` → `overlay_draw` → ImGui render → present, lines 644-658) could also become `app_draw_main(s)`. Extract it too, or stop here?
4. **Phase 4 verification confidence:** with no unit coverage for `app.cc`, is build + `make tidy` + manual `make run` an acceptable gate, or should a minimal headless smoke harness for `app_init`/`app_quit` (alloc/teardown without a visible window) be added first?
5. **Commit vs PR / worktree:** land these as four sequential commits on `main`, or isolate on a `refactor/top3-smells` branch (or per-phase worktrees, since Phases 1 & 4 are parallelizable)?
