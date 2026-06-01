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

## Unresolved questions

1. `INFOGAIN_BAND` (engine candidate band) and whether to share or split `HEUR_BAND` —
   start both ~0.02–0.05; tune in measurement.
2. Should `Inf(x)` extend across components via the global mine budget (second-order),
   or stay per-component (the paper's Φ-local definition)? Start per-component; revisit
   only if the gain underwhelms.
3. Tie-break order (info_gain → cascade → row-major) vs a weighted blend of info_gain
   and cascade — start lexicographic; blend only if measurement motivates it.
