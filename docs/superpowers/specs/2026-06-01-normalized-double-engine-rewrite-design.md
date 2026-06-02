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

**Peak-normalized binomial-weight table**, pure integer-ratio recurrence (no
`lgamma`/`log` → reentrant, no `signgam` race; matches existing `binom_ld`
style). Rejected alternatives: log-domain via `lgamma` (signgam reentrancy
hazard, needless transcendentals); inline single-reference factor-out (doesn't
robustly avoid tail overflow, leaves the perf bug).

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

Built **once per analyze**, in `compute_interior_prob`, before the
binom-consuming loops, over the window of `j` the loops query
(`j = r_eff − f` in zsum; `j = r_eff − k − t` in `ac[k]`):

- `base = max(0, r_eff − (MAXFLEN−1) − MAX_COMP_VARS)`, `jhi = min(N, r_eff)`,
  `span = jhi − base + 1` (≤ array size; union of both consumers' query ranges).
- reference `jstar = clamp(N/2, base, jhi)` — the window argmax (`C(N,·)` is
  unimodal), so **every `w ≤ 1`** (no overflow).
- `w[jstar−base] = 1`; fill upward `× (N−j)/(j+1)`, downward `× j/(N−j+1)` — the
  same recurrence `binom_ld` uses, pure multiply/divide.
- accessor `binw(table, j)` → `w[j−base]` if `base ≤ j < base+span`, else `0`
  (correct: `C` is 0 outside `[0,N]`; far-tail entries underflow to 0 harmlessly).

Consumers multiply `binw(...)` where they multiplied `binom_ld(...)`:
`zsum`, `interior_num` (`compute_interior_prob`), `ac[k]` (`write_exact_marginals`).
All three share the implicit `C(N,jstar)` factor, which **cancels** in
`num/zsum` and `interior_num/(zsum·N)` → results identical to today minus the
overflow. Build O(span); lookups O(1) → the per-call hotspot disappears.

The table lives in scratch (per-instance) → reentrant.

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
| `MAXFLEN` | 256 | 500 | **1024** | gates `exact_ok` only (storage, not per-op cost); generous tail margin; ~+1 MB. |
| `MAX_COMP_VARS` | 64 | 102 | **128** | covers observed 102 + margin; the only cap with real RREF time cost → gated by the §"Validation" time check. |
| `MAX_RED_ROWS` | 128 | — | **256** | a 128-var component can have >128 constraint rows. |

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

## Testing & validation

- `engine_test` (oracle, 1e-6) and `engine_reduction_test` (reduced ≡ direct):
  green, unmodified.
- `golden_test`: re-run; integer fields (`eval`, `best`, `forced_*`) are the
  hard gate and must not move. If float low bits shift past 1e-9 from the
  normalized arithmetic, **regenerate** the baseline (`GOLDEN_CAPTURE=1`) and
  note it as an intentional change; investigate any integer-field move.
- **New large-board regression test:** a 100×100/2500 state with large
  `interior_n` (the case today's `double`-naive build dies on at move 1) →
  assert all `mine_prob` finite and ∈[0,1] and a valid recommended move exists.
  Permanent guard for the overflow fix.
- **Time-validation gate (heavy-tail check):** after the cap raise, re-run the
  bounded profiler at 100×100/2500 and confirm worst-case per-move analyze stays
  bounded. If `MAX_COMP_VARS=128` reintroduces an RREF heavy tail, dial back to
  96 (covers p99=83).
- **`double`-build / portability check:** the `double` engine must now survive
  the 100×100/2500 workload end-to-end (what valgrind and MSVC see) — i.e. the
  proof experiment now passes instead of dying at move 1.

## Sequencing

1. **Stage A — correctness/perf/portability** (shippable alone): binomial-weight
   table + `long double`→`double`. All existing tests green; new regression test
   added; profiler confirms the hang is gone.
2. **Stage B — coverage extension:** raise the storage caps + OOB fix;
   re-validate per-move time and exact-coverage rate.

## Decisions (resolved)

- Approach: peak-normalized weight table (not log-domain, not inline factor-out).
- `long double` → `double` is in scope (pays for the caps; proven sufficient).
- `MAXFLEN = 1024`, `MAX_COMP_VARS = 128`, `MAX_RED_ROWS = 256`,
  `MAXCOMP = 128` (unchanged).
- Search budgets unchanged.

## Unresolved questions

1. `MAX_COMP_VARS=128` final vs falling back to 96 — deferred to the Stage B
   time-validation gate (empirical).
2. Whether the overlay's "exact" readout copy needs wording tweaks now that
   `exact=true` is common at large sizes (cosmetic; out of engine scope).
