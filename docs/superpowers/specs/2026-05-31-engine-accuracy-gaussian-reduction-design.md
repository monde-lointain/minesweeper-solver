# Engine Accuracy — Gaussian-Elimination Reduction (as built)

Date: 2026-05-31
Status: implemented (branch `feat/engine-accuracy-gaussian`).
Supersedes the rollout-first direction (see
`2026-05-31-smarter-guess-policy-design.md`, now deferred) after Liu et al. 2022
(*Knowledge-Based Systems* 246) showed Expert ~45% is reachable with **no
lookahead** — just stronger exact deduction + min-prob guessing.

## Problem

The engine fell back to a crude per-var average (`rem/unknown`) the moment a
connected frontier component exceeded `CAP_VARS=24` unknowns — the documented
Expert calibration drift. On Expert ~6.8% of analyze calls hit ≥1 such component.

## Design (implemented)

Per component, when `CAP_VARS < nv ≤ MAX_COMP_VARS`, solve it **exactly** by
Gaussian elimination instead of falling back:

1. **RREF** the `{0,1}`-coefficient constraint system with **exact rational
   arithmetic** (`src/engine/rational.h`: int64 num/den, `__int128`
   intermediates, gcd-reduce-then-range-check, overflow→invalid sentinel).
   Partial pivoting; identify leading vs **free** variables.
2. **Enumerate the free variables only** (`nv − rank`) via an incremental DFS:
   maintain each pivot row's partial value, prune the instant a leading var's
   reachable interval excludes `{0,1}`, and at the leaf back-substitute leading
   vars (must be exactly 0/1). Accumulate into the same `ec.sol/ec.mine` the
   direct path uses → the combine DP and `Analysis` contract are untouched.
3. Any **overflow / infeasible row / too-many-free / node-budget** condition
   aborts to the existing naive fallback — a wrong marginal is never emitted.

**Storage:** per-component results are **compact ragged** — flat arrays indexed
by per-component offsets, sized by `Σ nv_c ≤ MAXCELL` (mhat ~0.5 MB), so the
`MAX_COMP_VARS=64` ceiling costs no dense-array blowup.

**Fix A is per-component.** A cell safe in all *local* solutions is safe in all
*global* solutions, so an exact component's 0/1 is a real proof and is kept; only
fallback-component cells (and the whole board when the exact DP did not run) are
clamped off `{0,1}`. Reduction makes mixed exact+fallback boards common, where the
old board-global clamp discarded the reduction's forced cells.

**Knobs:** `MAX_COMP_VARS=64` (storage ceiling — covers 99.5% of fallback
components, max observed nv=88), `FREE_CAP=28` (search), `REDUCED_NODE_BUDGET=30000`
(per-component DFS cap; rational ops make a larger budget too slow for the tail).

## Verification

- `rational_test` — identities, normalization, overflow-flagged, 20k-case
  property check vs `__int128` reference.
- `engine_reduction_test` — **differential** (force-reduce ≡ direct enumeration,
  ≤1e-12, via the `engine_internal.h` test hook), **global-budget identity**
  (Σ P(mine) == mines, 1e-6 on exact boards), **forced-flag consistency**.
- `engine_test` exact oracle **unchanged** (direct path for nv≤24 untouched).
- `golden_test` **re-baked** (dense boards now solved exactly — far more forced
  cells; e.g. `small_e` fully deduced).
- Gates: gcc `-Werror -Wpedantic` and clang-22 + Orthodoxy, all green.

## Results (baseline min-prob policy, NEW vs OLD engine, same seeds)

| Difficulty | Games | OLD | NEW | Δ |
|---|---|---|---|---|
| Expert (30×16, 99) | 50k | 0.290 | **0.380** | **+9.0pp** |
| Intermediate (16×16, 40) | 10k | 0.718 | **0.727** | +0.9pp |
| Beginner (9×9, 10) | 10k | 0.889 | **0.902** | +1.3pp |

Expert forced-guess rate 0.040→0.027 (more cells deduced exactly);
`deaths@forced-safe` = 0 across all. Cost: analyze 110µs→~650µs mean (rational
RREF), max bounded ~0.5s by the reduced node budget. See the baseline report.

## Cheap guess heuristic (`POLICY_HEURISTIC`, secondary)

Min-prob + a progress tie-break, behind `--policy heuristic`; default stays
`baseline`. **First design regressed** (broad open-far/interior bias → isolated
reveals → more guesses: Expert old 0.290→0.241, new 0.383→0.363). **Re-scoped** to
reward **frontier connectivity** (resolve cells participating in many
constraints — a cheap `Inf(x)` info-gain proxy) + cascade likelihood, with a
small risk band (`HEUR_BAND=0.02`): beats baseline consistently (30k paired,
same boards: **0.383→0.390, +0.75pp**, deaths 0). Lesson: the paper's "open-far"
helps only for *unconstrained* guesses; as a broad tie-break it hurts.

## Why 0.38, not the paper's 0.456

0.38 is mid-range for published strong solvers (~38–42%); 0.456 is high, so part
of the gap is likely testbed. The solver-side gap is **guessing, not exactness**:
raising the exact-solve budget generously (`FREE_CAP` 28→40, node budget 30k→300k)
moved Expert only +0.2pp (within CI) at 8× the max latency — residual fallback on
high-entropy frontiers barely matters. The remaining headroom is principled
guess selection (`Inf(x)` re-deduction info-gain, or the deferred MC rollout),
which a cheap tie-break only partly captures.

## Deferred / future

- Sampler + Monte-Carlo rollout (prior spec) — revisit only if accuracy leaves a
  gap worth the cost.
- Integerized (fraction-free) reduced enumeration to cut the rational-op cost and
  raise the node budget / ceiling.
- Components with `nv > MAX_COMP_VARS` (0.5%, up to nv=88) still fall back.
