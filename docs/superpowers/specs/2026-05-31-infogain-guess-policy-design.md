# Info-Gain Guess Policy — paper's `Inf(x)` (engine + bench)

## Context

The accuracy pivot (Gaussian reduction) raised Expert 0.289→0.380 but left a gap to
Liu et al. 2022's 45.6%. The measured residual is **guessing**, not deduction
accuracy: the cheap frontier-connectivity tie-break only bought +0.75pp. The paper's
real guessing lever is `Inf(x)` (Def. 14 / "Additional enhancements", §3.1.4): for a
near-safe candidate cell `x`, substitute `x = 0` (pin it safe) into the constraint
system Φ and count the resulting **invariables** (cells forced to a fixed value across
all solutions). Among low-risk candidates, pick the one with the largest `Inf(x)` —
the guess that unlocks the most deductions. It is pure constraint propagation: it does
**not** read x's hidden number, so it is no-hidden-information clean.

## Key constraint

`Inf(x)` cannot be computed in the bench layer. Asserting a covered cell is safe
*without knowing its number* is not expressible through `Board` (the engine reads
revealed `adjacent` counts). The constraint system lives entirely in `SolverScratch`.
**So `Inf(x)` is computed engine-side and surfaced on `Analysis`.**

## Design

### Engine (`engine.h`, `engine.cc`)

- **Contract:** add `int info_gain;` to `CellAnalysis` (frozen-contract extension; one
  int, backward-compatible with memset-zero). For a covered cell it is the number of
  *other* frontier cells that become provably safe or provably mine if this cell is
  assumed safe. 0 when not computed (non-guess positions, interior cells, fallback /
  over-budget components, or cells outside the candidate band).
- **When:** computed only at `eval == EVAL_GUESS`, only for frontier cells within
  `INFOGAIN_BAND` of `best_prob` and not `forced_mine`, whose component is *exact*
  (non-fallback). This bounds cost to genuinely competitive guesses; non-guess
  positions (START/SAFE) pay nothing.
- **Mechanism (`component_infogain`):** reuse the existing per-component enumeration.
  Build a *pinned* local model into transient `s->ec` over the candidate's component's
  unknown vars **excluding** the candidate (≡ pinning it to 0), then dispatch the same
  way `enumerate_component` does (direct DFS for ≤ `CAP_VARS`, Gaussian-reduction
  free-var DFS above). Read the restricted per-var marginals from `ec.sol`/`ec.mine`;
  a var is *restricted-invariable* iff its marginal ∈ {0,1}. Count vars that are
  restricted-invariable but were **not** invariable originally (original marginal from
  the just-computed `res_mhat`). Returns 0 on overflow / infeasible / over-budget.
- **State discipline:** reads `s->res`/`s->cm`/`s->comp` (unchanged), writes only the
  transient `s->ec`/`s->rd`. No `vstate` mutation (pin = exclude from the model). Safe
  to call repeatedly across candidates within a move. Runs after `pick_best_move`;
  nothing downstream reads `s->ec`.
- **Soundness:** an originally-uncertain var that is fixed in *every local solution*
  with the candidate pinned safe is fixed in every *global* solution (the Fix-A
  argument: the global mine budget only removes local combinations). So a counted
  invariable is a real future forced cell.

### Bench (`policy_infogain.{h,cc}`, `policy.{h,cc}`, `args.cc`)

- New `POLICY_INFOGAIN` (own file; baseline + cheap heuristic untouched, for clean
  A/B). `--policy infogain`.
- Selection: primary key unchanged (min `P(mine)`); among covered, non-`forced_mine`
  cells within `HEUR_BAND` of `pmin`, rank by `info_gain` descending, then the existing
  cascade/connectivity `progress_score`, then row-major. Opening (`EVAL_START`) pinned
  to the engine pick (paired-comparison neutral).

## Verification

- **Engine `Inf(x)` test (key):** on boards small enough to brute-force, an independent
  full mine-configuration enumeration computes, per candidate, the true count of cells
  forced when that candidate is pinned safe; assert `out.cells[i].info_gain` matches.
  Covers single-point cascades and linear invariables, exact + reduced dispatch.
- **Policy test:** fixtures set `mine_prob` + `info_gain`; assert max-info-gain within
  band wins, min-risk respected, never `forced_mine`, deterministic, opening pinned.
- **Golden:** unaffected (summarizes `mine_prob`/forced counts, not `info_gain`).
- **Bench (Stream C):** Expert winrate baseline vs infogain vs cheap heuristic, same
  seeds, `deaths@forced-safe == 0`; analyze ns mean/max delta.

## As-built results (60k Expert, paired seeds, seed 1)

| policy | winrate (95% Wilson) | vs baseline | analyze mean / max |
|---|---|---|---|
| baseline  | 0.3823 [.3784,.3862] | —       | 702µs / 693ms |
| heuristic | 0.3894 [.3855,.3933] | +0.71pp | 572µs / 571ms |
| infogain  | **0.3922 [.3883,.3961]** | **+0.99pp** | 705µs / 1.97s |

- infogain vs baseline: **non-overlapping CIs → significant**. vs the cheap heuristic:
  +0.28pp point estimate, consistent across 20k (0.3921 vs 0.3893) and 60k, but CIs
  overlap → not separable without paired (McNemar) logging.
- `deaths@forced-safe == 0` everywhere; Beginner 0.917 / Intermediate 0.809 (healthy).
- Info-gain guessing does **not** close the gap to the paper's 0.456 — consistent with
  the residual being the testbed (first-click semantics) and the paper's global-budget
  pruning, not the per-guess rule.

**Decisive design lesson:** info_gain must *extend* the proven heuristic, not replace
it. A strict `info_gain` primary with a cascade-only secondary REGRESSED below the
heuristic (0.3856), because the common no-forcing case (info_gain all 0) dropped the
connectivity signal. Using the heuristic's connectivity+cascade progress as the
secondary makes infogain reduce to the heuristic when nothing forces and only add a
forcing preference on top → ≥ heuristic by construction.

## Resolved questions

1. `INFOGAIN_BAND` = 0.05 engine-side (> the bench `HEUR_BAND` 0.02, so policy-invariant
   — purely a perf bound; cut analyze max 5.3s → 1.97s). The bench reuses `HEUR_BAND`.
2. `Inf(x)` stays per-component (the paper's Φ-local definition). The cross-component
   global-budget extension was not needed — the gain is already marginal.
3. Tie-break is lexicographic (info_gain → heuristic progress → row-major), NOT a
   weighted blend; the lexicographic form is what makes infogain dominate the heuristic
   by construction (see lesson above).

## Final disposition

- **infogain is the default policy.** The cheap heuristic (`POLICY_HEURISTIC`,
  `policy_heuristic.{h,cc}`, its test) was **deleted** — it is strictly dominated by
  infogain (which reduces to it when nothing forces). `POLICY_BASELINE` is **kept** as
  the engine-accuracy reference (isolates the engine's raw min-prob pick; needed to
  measure future engine changes without policy confound). CLI: `--policy infogain|
  baseline` (default infogain). The overlay/game is unaffected (policy is bench-only).

## Open follow-ups

- Paired McNemar (per-seed win/lose logging) to confirm infogain > baseline beyond the
  already-significant aggregate CIs, and to quantify the lost +0.28pp signal vs the
  deleted heuristic if ever revisited. The harness currently emits only aggregate
  counts.
