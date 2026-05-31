# Solver Benchmark — Phase-1 Baseline

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
