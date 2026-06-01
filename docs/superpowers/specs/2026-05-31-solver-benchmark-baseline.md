# Solver Benchmark — Phase-1 Baseline

> **UPDATE (info-gain guess policy, branch `feat/engine-infogain`).** Added the
> paper's `Inf(x)` (engine `solver_analyze_infogain` + `--policy infogain`) — see
> `2026-05-31-infogain-guess-policy-design.md`. Expert, 60k paired seeds:
>
> | Policy | Expert winrate | 95% Wilson | vs baseline | analyze mean/max |
> |---|---|---|---|---|
> | baseline | 0.3823 | [.3784,.3862] | — | 702µs / 693ms |
> | heuristic (cheap) | 0.3894 | [.3855,.3933] | +0.71pp | 572µs / 571ms |
> | **infogain** | **0.3922** | [.3883,.3961] | **+0.99pp** | 705µs / 1.97s |
>
> infogain vs baseline is significant (non-overlapping CIs); vs the cheap heuristic
> it is +0.28pp (consistent at 20k & 60k) but CI-overlapping. `deaths@forced-safe
> 0`; Beginner 0.917 / Intermediate 0.809 (no regression). Lesson: info_gain must
> EXTEND the heuristic (lexicographic: info_gain → connectivity+cascade → row-major),
> not replace it — a cascade-only secondary regressed to 0.3856.
>
> **infogain is now the default policy**; the cheap heuristic was deleted (dominated
> by infogain). `POLICY_BASELINE` kept as the engine-accuracy reference. CLI:
> `--policy infogain|baseline` (default infogain).
>
> **UPDATE (Gaussian-reduction accuracy work, branch `feat/engine-accuracy-gaussian`).**
> Dense frontier components (`nv>24`) are now solved exactly by Gaussian
> elimination instead of the naive fallback — see
> `2026-05-31-engine-accuracy-gaussian-reduction-design.md`. New baseline-policy
> winrates (same seed base, NEW vs the original engine below):
>
> | Difficulty | Games | OLD | NEW | 95% Wilson (NEW) | deaths |
> |---|---|---|---|---|---|
> | Beginner | 10k | 0.889 | **0.902** | [0.896, 0.907] | 0 |
> | Intermediate | 10k | 0.718 | **0.727** | [0.718, 0.736] | 0 |
> | Expert | 50k | 0.283/0.290 | **0.380** | [0.376, 0.385] | 0 |
>
> Expert forced-guess 0.040→0.027 (more cells deduced exactly). Cost: analyze
> 110µs→~650µs mean (rational RREF; `REDUCED_NODE_BUDGET=30000` bounds the tail
> ~0.5s).
>
> **Why 0.38 and not the paper's (Liu et al. 2022) 0.456?** 0.38 sits squarely in
> the published strong-solver range (~38–42%); 0.456 is at the high end, so some of
> the gap is likely their testbed/measurement. The solver-side gap is **guessing,
> not exactness**:
> - *Exactness is not the lever.* Raising the exact-solve budget generously
>   (`FREE_CAP` 28→40, node budget 30k→300k) moved Expert only 0.383→0.385 (+0.2pp,
>   within CI) while blowing max analyze time to 4.2s. Residual fallback on
>   high-entropy frontiers contributes ~nothing — exact probabilities there don't
>   make the guess easier.
> - *Guessing is the lever, but cheap tie-breaks give a little.* `POLICY_HEURISTIC`'s
>   first design (broad open-far/interior bias) **regressed** Expert (old 0.290→0.241,
>   new 0.383→0.363 — isolated reveals → more guesses). Re-scoped to reward
>   **frontier connectivity** (resolve cells in many constraints) + cascade, with a
>   small risk band (`HEUR_BAND=0.02`), it now beats baseline consistently — 30k
>   paired (same boards): **0.383 → 0.390 (+0.75pp)**, deaths 0. Available via
>   `--policy heuristic`; default stays `baseline` so the headline measures the
>   engine alone. The paper's larger guessing edge needs principled info-gain
>   (`Inf(x)` re-deduction) or lookahead (the deferred MC rollout).
>
> The numbers below are the ORIGINAL pre-reduction baseline, kept for reference.

Date: 2026-05-31
Harness: `minesweeper_bench` (Stream A–C), `POLICY_BASELINE` (engine min-prob pick).
Build: gcc `-Werror` (clang-22/Orthodoxy gate separate). Determinism verified
(identical aggregate at `--threads 1` vs `4/8/16`).

## Headline numbers

| Difficulty | Dims (mines) | Density | Games | Winrate | 95% Wilson | deaths@forced-safe |
|---|---|---|---|---|---|---|
| Beginner | 9×9 (10) | 12.3% | 2000 | **0.889** | [0.874, 0.902] | 0 |
| Intermediate | 16×16 (40) | 15.6% | 5000 | **0.718** | [0.705, 0.730] | 0 |
| Expert | 30×16 (99) | 20.6% | 5000 | **0.283** | [0.270, 0.295] | **3 ⚠** |

Reproduce: `minesweeper_bench --difficulty <d> --games <n> --seed 1`.

## Guessing profile

| Difficulty | forced-guess (of deduced) | mean guess risk | guess survival | mean loss depth |
|---|---|---|---|---|
| Beginner | 4.95% | 0.106 | 0.879 | 0.25 |
| Intermediate | 2.38% | 0.165 | 0.828 | 0.47 |
| Expert | 3.64% | 0.186 | 0.837 | 0.41 |

`mean loss depth` = revealed fraction when a game is lost. Expert loses ~40% of
the way in, i.e. survives many safe cascades then dies on a guess (or the defect
below).

## Calibration (chosen guesses)

Beginner / Intermediate: predicted P(mine) tracks empirical hit-rate closely
across populated buckets — the exact engine is well-calibrated where it runs
exactly.

Expert: the 50/50 bucket is exact (pred 0.500 / emp 0.503), but mid buckets drift
— the engine **overestimates** risk in [0.20,0.40] (e.g. pred 0.265 vs emp 0.163),
consistent with the naive fallback firing on large frontiers. Combined with the
forced-safe defect (underestimation to 0), this confirms fallback-path
inaccuracy as the dominant Expert error source the spec predicted.

## ⚠ Engine correctness defect found (out of Phase-1 scope)

The harness's correctness gate fired immediately: **Expert produces 3 forced-safe
deaths in 5000 games** (seed 1) — the engine assigned `forced_safe`/P(mine)≈0 to
cells that were actually mines. Properties:

- **Deterministic**: identical count at `--threads 1` and `--threads 16` → a real
  engine bug, not a harness race.
- **Location (suspected)**: the naive fallback / large-component combination path
  (`enumerate_all` → `fallback_component` / global DP), where a spurious exact-0
  marginal can be produced for a covered cell that is a mine in some solutions.
- **Impact**: a forced-safe death is an *unavoidable, mispriced* loss — the policy
  cannot avoid it because the engine reports the cell as proven safe. It both
  loses games and corrupts any guess strategy built on the probabilities.

This is captured as a disabled repro test
(`BenchIntegration.DISABLED_ExpertNeverKillsOnForcedSafe`); enable once fixed.

**UPDATE (Fix A applied):** root-caused to `compute_interior_prob` collapsing each
fallback component's mine-count distribution into a *rounded point estimate*
(`r_eff = rem_mines - llroundl(fallback_expected)`), which starves the global mine
budget and lets the exact DP / uniform-interior formula emit `mine_prob` exactly
0/1 for undetermined cells. Fix A (`write_cell_probs`): in any analysis that used
approximation, clamp non-deduced cells away from {0,1} so approximations can never
read as proofs. Result: `deaths@forced-safe` → 0; Expert winrate unchanged within
CI (0.282). The regression test is now enabled.

**UPDATE (Fix B, minimal):** removed the rounded point estimate entirely in the
`exact_ok` DP path. Each fallback component now enters the global convolution DP
as its mine-count *distribution* (Poisson-binomial of the naive per-var `fb_p`,
`build_fbdist`), with the full budget `r_eff = rem_mines` — so the interior and
exact-component marginals reflect true uncertainty instead of a collapsed mean.
Identity when nothing falls back (exact-path oracle `engine_test` still passes at
1e-6). Result over Expert 50k: **winrate 0.283 → 0.289**, **guess survival
0.837 → 0.855** (chosen guesses are genuinely safer — better-ordered
probabilities), `deaths@forced-safe` still 0. Residual mid-bucket calibration
drift remains (it comes from the naive per-var `fb_p`, untouched by minimal B);
improving those marginals is "full B" / future work.

## Performance

Expert: 1086 games/s on 16 threads; analyze 110 µs mean, 30 ms max. The 1M-game
default ≈ 15 min on 16 cores. Intermediate ~5500 games/s; Beginner ~24k games/s.

## Data-driven next steps (for the strategy brainstorm)

Priority order suggested by the data:

1. **Fix the forced-safe defect (correctness, prerequisite).** Probabilities must
   be sound before any guess strategy is meaningful. Requires touching the engine
   (frozen contract) → its own scoped change.
2. **Tighten Expert probability accuracy.** Reduce fallback firing: raise
   `CAP_VARS`, add subset/constraint reduction, or a better large-component
   approximation. The calibration drift quantifies the headroom.
3. **Smarter guess selection.** Survival ~0.83 per guess and low forced-guess
   rate mean each guess is costly; information-gain / progress-probability
   tie-breaking over equal-risk cells is the lever once probabilities are sound.

100×100 / 2440-mine target remains **Phase 2** (board cap raise).
