# Normalized-`double` engine rewrite — design

## Problem

`solver_analyze` effectively hangs / produces garbage at the raised game limits
(≤100×100, ≤2500 mines). Two coupled defects:

1. **Numeric overflow / perf.** `binom_ld(n,k)` computes raw `C(interior_n, j)`.
   At 100×100/2500, `interior_n`≈thousands → `C(~7500,~2400) ≈ 1e2000+`. It
   survives only in 80-bit `long double` (range ~1e4932); it overflows 64-bit
   `double` (~1e308). And it is recomputed in O(min(k,n−k)) per call, millions of
   times per analyze (~97% of analyze time) — the "hang".
2. **Coverage caps tuned for expert.** `MAXFLEN=256` / `MAX_COMP_VARS=64` are
   exceeded routinely at 100×100, dropping boards to the crude approximation.

## Root cause (proven)

80-bit *precision* is **not** needed; only exponent *range* is.

- Engine rebuilt with `double`: `engine_test` (vs brute-force oracle, tol 1e-6)
  and `golden_test` (vs long-double baseline on dense expert boards, tol 1e-9)
  both **pass**. The public API (`Analysis.mine_prob`) is already `double`.
- The huge binomials **cancel**: every output is a ratio
  (`mine_prob = num/zsum`, `interior_prob = interior_num/(zsum·interior_n)`)
  where every term carries the same `C(interior_n, j)` scale.
- `double` build dies at move 1 on 100×100/2500 *exactly* like valgrind and like
  MSVC (`long double == double` there) — confirming the dependency is range, and
  that the Windows build is **already broken** at large sizes today.

So `binom_ld` is the *only* overflow source; everything else
(`ec.sol`/`mine` counts ≤ node budget; normalized `shat`/`mhat`∈[0,1]; `Poly`
convolutions of normalized dists) stays well inside `double` range.

## Approach (chosen)

**Dominant-term-normalized binomial-weight table.** The hot per-element path is
a pure integer-ratio recurrence (no transcendentals; matches existing `binom_ld`
style); a single cold `O(wall.len)` pass uses plain `log()` only to locate the
normalization anchor (the dominant term of the sum). `log()` is reentrant — the
`signgam` hazard is specific to `lgamma`, which we avoid. Rejected alternatives:
full log-domain via `lgamma` per element (signgam hazard, transcendentals in the
hot path); a fixed `N/2` geometric anchor (over/underflows when the weighted
support is far from `N/2`); inline single-reference factor-out (doesn't robustly
avoid tail overflow, leaves the perf bug).

## Changes

### 1. Binomial-weight table (core fix) — replaces `binom_ld`

New POD in `SolverScratch`:

```c
struct BinTable {
  double w[MAXFLEN + MAX_COMP_VARS]; /* w[i] = C(N, base+i) / C(N, jstar) */
  int base;  /* j of w[0] */
  int span;  /* valid entries */
  int N;     /* = interior_n for this analyze */
};
```

Built **once per analyze, only when `exact_ok`**, in `compute_interior_prob`
*after* `wall` is built and before the binom-consuming loops, over the window of
`j` the loops query (`j = r_eff − f` in zsum; `j = r_eff − k − t` in `ac[k]`):

- `base = max(0, r_eff − (MAXFLEN−1) − MAX_COMP_VARS)`, `jhi = min(N, r_eff)`,
  `span = jhi − base + 1` (≤ array size; union of both consumers' query ranges).
- **Reference = the dominant term of the actual sum**, not a geometric peak:
  `jstar = r_eff − f*`, where `f* = argmax_f [ wall[f] · C(N, r_eff−f) ]`. Found
  by one cold `O(wall.len)` pass in the log domain — `lw[f] = log(wall[f]) +
  logC(r_eff−f)`, `logC` accumulated from the `O(1)` ratio `log((N−j+1)/j)` —
  taking the **full-scan** max (do *not* assume unimodality: `shat` need not be
  log-concave, so `wall` need not be either). `log()` is used *only* in this cold
  pass and is reentrant (unlike `lgamma` it never touches `signgam`); the hot
  per-element lookups stay pure-multiply. Anchoring on the dominant term makes
  the largest weighted contributor exactly `1` and every significant term
  `O(1)` — eliminating **both** failure modes a fixed `N/2` anchor admits:
  overflow (anchor too low) and catastrophic `zsum→0` underflow (anchor far from
  the weighted support).
- `w[jstar−base] = 1`; fill upward `× (N−j)/(j+1)`, downward `× j/(N−j+1)` — the
  same recurrence `binom_ld` uses, pure multiply/divide. Clamp any `w` to the
  finite `double` range on build (a `w` that would overflow belongs to a
  negligible-weight tail term, so clamping shifts the sum by < ulp; never store
  `inf`/`nan`).
- accessor `binw(table, j)` → `w[j−base]` if `base ≤ j < base+span`, else `0`
  (`C` is 0 outside `[0,N]`). The sum loops keep their existing `weight==0 →
  skip` guards, so a clamped/zero `w` is never multiplied into a `0·inf = nan`.

Consumers multiply `binw(...)` where they multiplied `binom_ld(...)`:
`zsum`, `interior_num` (`compute_interior_prob`), `ac[k]` (`write_exact_marginals`).
All three share the **identical** `C(N,jstar)` factor — one table, built once,
**persisted unchanged between the two functions** — which **cancels** in
`num/zsum` and `interior_num/(zsum·N)` → results equal today's minus the
overflow. Build O(span) + one O(wall.len) reference pass; lookups O(1) → the
per-call hotspot disappears.

The table lives in scratch (per-instance) → reentrant. Because
`wall = shat_c ⊗ oc2`, `zsum`'s dominant term sits in the same hump as every
component's `ac` dominant term, so the shared `jstar` keeps `ac`'s terms `O(1)`
too.

### 2. `long double` → `double` (internal)

Switch the ~50 internal sites: `Poly.v`, `shat_flat`/`mhat_flat`/`fb_p_flat`,
`ec.sol`/`ec.mine`, the `res_*` accessor return types, the convolution/combine
accumulators, `EPS` (`1e-9L`→`1e-9`), `solver_clamp01` (util.h), `binom_ld`
(removed for the table), `llroundl`→`llround`. Rationale: proven sufficient;
matches the `double` public API; and **halves the float arrays, paying for the
bigger caps at ~constant memory**.

**Untouched:** the rational reduction path (`engine_reduce.cc`, `rational.h`) is
int64/int128 — float-type-independent, already MSVC-portable.

### 3. Storage caps (evidence-backed)

Sized from a 46,444-analysis sample over ~97 games at 100×100/2500 under the
production info-gain policy:

| cap | today | observed max | **new** | rationale |
|---|---|---|---|---|
| `MAXCOMP` | 128 | 44 | **128 (unchanged)** | 3× headroom; frontier is few-but-large, never many small comps. |
| `MAXFLEN` | 256 | 500 | **1024** | gates `exact_ok`; ~+1 MB storage. Per-conv cost uses live length, *but* admitting ~1000-mine frontiers into the full combine DP (prefix/suffix + per-comp `oc`/`ac`) adds ~few×10⁶ double ops on those boards — bounded (~ms), checked by the time gate. |
| `MAX_COMP_VARS` | 64 | 102 | **128** | covers observed 102 + margin; the only cap with real RREF time cost (gcd-heavy rational elimination runs to completion even when the component later falls back) → **go/no-go** time gate, not mere tuning. |
| `MAX_RED_ROWS` | 128 | — | **256** | coverage heuristic, not a guarantee: a 128-var component *can* exceed it, in which case it falls back (safe), just uncovered. |

**Search budgets (`NODE_BUDGET`, `REDUCED_NODE_BUDGET`, `FREE_CAP`) stay
unchanged** — raising them is a known heavy-tail trap (~11s worst-case analyze).
Raising `MAX_COMP_VARS` only lets bigger components *attempt* the
budget-bounded reduction (succeed if they reduce to `nfree ≤ FREE_CAP`, else
fall back as today).

### 4. `ncomp > MAXCOMP` OOB defensive fix

`write_fallback_probs` and the `any_fb` loop in `solver_analyze` iterate
`c ∈ [0, ncomp)` and index `res.nv[c]`/`res.fallback[c]` (sized `[MAXCOMP]`).
`ncomp > MAXCOMP` is out-of-bounds at HEAD (latent raised-size bug; unreached in
practice since observed `ncomp ≤ 44`). Clamp both loops to `min(ncomp, MAXCOMP)`
unconditionally (the over-cap board is already `exact_ok=false` / crude, so the
clamped comps just keep their baseline probs).

## Memory budget

At `double` + new caps: scratch ≈ **~15 MB** (vs ~13 MB today at `long double`).
Dominated by `mhat_flat = MAXCELL×(MAX_COMP_VARS+1)×8 ≈ 10.3 MB`. RSS stays
~2.5 MB (the flats are touched sparsely — observed `ncomp ≤ 44`, `nv ≤ 102`).
Acceptable for the overlay (1–2 scratches) and the threaded bench
(N × ~15 MB).

## What does NOT change

- Algorithm: constraint build, single-point deduction, components, direct/
  reduced enumeration, the leave-one-out convolution DP — same structure and
  accumulation order (golden-pinned).
- The `exact_ok` bail (frontier ≥ MAXFLEN or `ncomp > MAXCOMP`) and the naive
  fallback remain as the safety net for the rare over-cap tail.
- The rational reduction path and all `rational.h` arithmetic.
- `out->exact` *meaning* is unchanged; it will simply read `true` more often at
  large sizes now that more boards solve exactly (honest, not a regression).

## Side benefits (now true)

- **Cross-platform determinism improves.** Uniform `double` replaces x86-only
  80-bit `long double`, so results match on MSVC/valgrind, and the `golden_test`
  baseline — currently implicitly x86-specific — becomes portable.
- **Solution counts stay exact in `double`.** Counts are bounded by the node
  budgets (≤ 5×10⁶ ≪ 2⁵³), so `shat = sol/total` loses no precision —
  independent confirmation that `double` suffices for the *counting* machinery,
  not just the final ratio. Convolution sums are all-nonnegative (no
  cancellation); accumulated relative error ~1e-13, well under the 1e-9/1e-6
  targets.

## Testing & validation

- `engine_test` (oracle, 1e-6) and `engine_reduction_test` (reduced ≡ direct):
  green, unmodified.
- `golden_test`: re-run; integer fields (`eval`, `best`, `forced_*`) are the
  hard gate and must not move. If float low bits shift past 1e-9 from the
  normalized arithmetic, **regenerate** the baseline (`GOLDEN_CAPTURE=1`) and
  note it as an intentional change; investigate any integer-field move.
- **New large-board liveness test:** a 100×100/2500 state with large
  `interior_n` (the case today's `double`-naive build dies on at move 1) →
  assert all `mine_prob` finite and ∈[0,1] and a valid recommended move exists.
  Permanent guard against overflow/`nan` — but liveness only, **not** value
  correctness (see next).
- **Large-N *correctness* oracle (closes the no-oracle-at-scale gap):** the
  brute-force oracle and the golden baseline don't certify values in the
  overflow regime. Add a check that isolates *normalization* correctness from
  precision: compute the same engine in `long double` **using the new weight
  table** vs `double` using the new weight table, on a corpus of dense
  large-`interior_n` states, and assert agreement to ~1e-9. (The `long double`
  reference can't overflow there, so any divergence is a table/cancellation
  bug, not a range artifact.) Complement with at least one **symmetry board**
  whose interior probability is analytically known by construction, asserted
  directly. This is a **gate on #1**.
- **Time-validation gate — concrete go/no-go (heavy-tail check):** after the cap
  raise, re-run the bounded profiler at 100×100/2500 (info-gain). **Pass iff
  p99 per-move analyze ≤ 50 ms and max ≤ 250 ms** (interactive-overlay
  tolerance). If `MAX_COMP_VARS=128` violates it, fall back to 96 (covers
  p99 component size = 83) and re-measure; 96 is acceptable, 128-with-heavy-tail
  is not.
- **`double`-build / portability check:** the `double` engine must now survive
  the 100×100/2500 workload end-to-end (what valgrind and MSVC see) — i.e. the
  proof experiment now passes instead of dying at move 1.

## Sequencing

1. **Stage A — correctness/perf/portability** (shippable alone): binomial-weight
   table + `long double`→`double`. Gates: all existing tests green; new liveness
   + large-N correctness oracle pass; profiler confirms the hang is gone.
2. **Stage B — coverage extension:** raise the storage caps + OOB fix. Two
   explicit **go/no-go** gates (not tuning):
   - **Time:** the §Testing time gate (p99 ≤ 50 ms, max ≤ 250 ms). Fail → drop
     `MAX_COMP_VARS` to 96 and re-measure.
   - **Value:** bench win-rate at 100×100/2500, cap-raise vs Stage-A-only. The
     raise must **not reduce** win-rate and should show a measurable
     decision-quality gain. If the crude fallback is within noise, **ship Stage A
     only** — the extra exactness isn't worth its cost. (We measured how often
     caps bind, never that binding *hurts outcomes*; this gate establishes that.)

## Decisions (resolved)

- Approach: dominant-term-normalized weight table — pure-multiply hot path,
  anchored by a cold `O(wall.len)` log-domain reference pass (not full
  log-domain, not `lgamma`, not a fixed `N/2` anchor, not inline factor-out).
- `long double` → `double` is in scope (pays for the caps; proven sufficient).
- `MAXFLEN = 1024`, `MAX_COMP_VARS = 128`, `MAX_RED_ROWS = 256`,
  `MAXCOMP = 128` (unchanged).
- Search budgets unchanged.

## Validation results (executed 2026-06-01)

Implemented on branch `engine-normalized-double`. All 71 tests green.

**Stage A — core (binomial table + `double`):**
- `engine_test`/`engine_reduction_test`/`engine_exact_test`/`engine_infogain_test`
  pass unmodified. `golden_test` passes **unmodified at tol 1e-9** (no rebaseline
  needed; integer fields identical) — the normalized-`double` engine reproduces
  the long-double baseline within tolerance.
- New `engine_largeN_test`: conservation (ΣP = mines, exact) and
  ld-with-table vs double-with-table agreement (≤1e-9) both hold at
  `interior_n`≈9990 where raw `C(9990,2500)`≈1e2900 overflows double. Closes the
  no-oracle-at-scale gap.
- Hotspot eliminated: profiler at 100×100/2500 — cap-600 3.62s→**0.06s (60×)**;
  full game 180s→12s; runs deep with no `nan`/`inf` (old `double`-naive died at
  move 1).

**Stage B — caps:**
- **Time gate PASS:** 54,842 single-thread analyze samples — p50 1.12ms, **p99
  22.78ms** (≤50), **max 49.49ms** (≤250), mean 4.32ms. No RREF heavy tail;
  `MAX_COMP_VARS=128` kept (no fallback to 96).
- **Value gate PASS (decisive):** 200 games each, 100×100/2500 infogain. Win-rate
  0/200 both (board is effectively unwinnable — wrong metric here), but Stage B:
  mean loss depth 0.1024→**0.1910** (+86%), guess survival 0.8086→**0.8684**,
  safe deductions 92.8k→**178.9k**, and the critical low-risk calibration bucket
  `[0,0.05)` went **pred 0.020 / empirical 0.380 (badly overconfident) → pred
  0.035 / empirical 0.032 (calibrated)**. The old caps' crude approximation
  mispriced near-safe cells and walked the policy into mines; exact coverage
  fixes it. Bench batch is ~5.5× slower (more exact work + deeper games) —
  acceptable for a dev tool; the overlay is one analyze/move at ≤50ms.

## Resolved / remaining questions

1. ~~`MAX_COMP_VARS=128` vs 96~~ — **128**, confirmed by the time gate.
2. ~~Whether Stage B ships~~ — **yes**, confirmed by the value gate.
3. Overlay "exact" readout copy now that `exact=true` is common at large sizes
   (cosmetic; out of engine scope) — still open, deferred to overlay work.
