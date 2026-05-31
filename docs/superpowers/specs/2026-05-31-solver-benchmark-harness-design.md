# Solver Benchmark Harness — Design Spec

Date: 2026-05-31
Status: approved (pre-implementation)

## 1. Goal & scope

Build a fast, SDL-free, deterministic harness that plays the solver against many
generated boards and reports **winrate + diagnostics** explaining *why* it loses.
This is the measurement foundation: a follow-up effort uses its output to devise
and validate winrate-improving strategies.

Phase 1 targets the supported board envelope (≤ 30×24 = `BOARD_MAX_CELLS`):
Expert (30×16, 99 mines) as the headline; Intermediate (16×16, 40) and Beginner
(9×9, 10) as sanity checks. **No engine changes.** The harness ships a
move-selection seam whose only policy reproduces today's behavior exactly.

Explicitly deferred:
- **Phase 2:** raise `BOARD_MAX_W/H/CELLS` + engine scratch to support the custom
  100×100 / 2440-mine target. Touches the frozen sibling `types.h` and grows
  `SolverScratch`; pushes the exact-enumeration caps far past their limits.
- **Strategy phase:** new policies / engine accuracy work, designed from the
  baseline data this harness produces.

## 2. Why these decisions

- **Harness first, strategy data-driven.** Can't devise or validate a strategy
  without a baseline and failure-mode data. The harness's diagnostics (forced-guess
  rate, guess calibration, where deaths occur) point at the right lever.
- **External calibration only.** The dominant suspected failure mode is the engine
  falling back to crude per-constraint averaging when a frontier component exceeds
  `CAP_VARS=24` (or `MAXCOMP`/`MAXFLEN` overflow). We measure the *effect* of that
  externally — recording `(predicted P, actual mine?)` per chosen guess — rather
  than instrumenting the engine. Keeps the frozen `engine.h` contract untouched.
- **30×24 first.** The game and engine are hard-capped at `BOARD_MAX_CELLS` (=720);
  100×100 cannot be created or analyzed without raising that cap. Sequenced as
  Phase 2 so the foundation lands first.

## 3. Architecture (build targets)

- **New `game_core` static lib** — `game.cc` + `util.cc` from `MINESWEEPER_DIR`
  (both verified libc-only: `game.cc` includes only `game.h` + `<string.h>`;
  `util.cc` only `<stdio.h>`/`<string.h>`). SDL-free, so the bench links no
  graphics stack. The existing `minesweeper_reuse` lib is *not* reused here — it
  drags in SDL3/mixer/imgui via render/assets/ui/audio.
- **New `minesweeper_bench` executable** — links `solver_lib` (engine +
  overlay_geom, already SDL-free) + `game_core`. No SDL. Orthodox C++ enforced
  (our code → Orthodoxy plugin + `-Werror`).
- **Module layout `src/bench/`** (mirrors `src/engine`, `src/overlay`):
  - `main.cc` — CLI parse + dispatch.
  - `policy.{h,cc}` — move-selection seam.
  - `runner.{h,cc}` — play loop, threaded fan-out, metric accumulation.
  - `metrics.{h,cc}` — Wilson CI, calibration buckets, summary formatting.
- Untouched: `app/`, `overlay/overlay.cc`, the engine, and all frozen sibling
  contracts (`types.h`, `game.h`, `engine.h`).

## 4. Play loop & determinism

```
game_reset(board, w, h, mines, Rng{fn=NULL, ctx=NULL, seed = base_seed + i})
loop while board.status is GAME_READY or GAME_PLAYING:
    solver_analyze(board, analysis, scratch)
    if policy_select(POLICY_BASELINE, board, analysis, &move) != 0: break  // no move
    game_reveal(board, move.x, move.y)        // flood-fill handles cascades
record outcome from board.status (GAME_WON / GAME_LOST)
```

- `Rng.fn == NULL` uses the board-local fallback LCG seeded by `seed` — reentrant
  and reproducible. Game `i` of a run uses `seed = base_seed + i`.
- First reveal places mines avoiding the click (existing first-click safety), so
  the opening move is never an immediate loss.
- One `SolverScratch` per worker thread, reused across that worker's games.
- Run output is a pure function of `(base_seed, difficulty/dims, games)` —
  independent of `--threads` (see §6).

## 5. Policy seam

```c
struct Move { int x; int y; };
enum PolicyId { POLICY_BASELINE = 0, POLICY_COUNT };

/* Choose a covered cell to reveal. Returns 0 and writes *out on success;
 * returns -1 if no covered cell exists. */
int policy_select(int id, const struct Board* b, const struct Analysis* a,
                  struct Move* out);
```

`POLICY_BASELINE` returns `a->best_x / a->best_y` (the engine's lowest-mine-prob
pick, row-major tie-break) — *exactly* today's behavior, so Phase-1 numbers are the
current solver's true winrate. Future strategies add `enum` cases without touching
the runner; a proven winner can later be promoted into the engine/overlay.

## 6. Threading & determinism

- `--threads N`, default = hardware concurrency (clamp ≥ 1; cap at a fixed max,
  e.g. 256). Concurrency uses `std::thread` + `std::atomic` (user-sanctioned for
  the bench), confined to `runner.cc`. SDL_Thread is rejected to keep the bench
  SDL-free.
- Work distribution: a shared `std::atomic<int>` next-game counter hands out game
  indices in `[0, games)` to workers (dynamic load balancing — game lengths vary,
  so static chunks would leave threads idle near the end). The atomic affects only
  *which* worker runs a game, never the game's outcome.
- Each worker owns a private `SolverScratch` and a private `Metrics` accumulator —
  no shared mutable state on the hot path, no locks. Workers are joined and their
  `Metrics` merged.
- Aggregate determinism: game `i`'s outcome depends only on its seed (`base + i`);
  every metric is a commutative reduction (sum / max), so the merged result is
  identical for any thread count or scheduling. Only stdout is printed, once, at
  the end.
- `bench_test` asserts identical aggregate results at `--threads 1` vs `--threads 4`.

## 7. Metrics & diagnostics (all external, no engine change)

Accumulated per run and reported at the end:

- **Winrate** + Wilson 95% confidence interval.
- **Forced-guess rate** — fraction of decisions with `eval == EVAL_GUESS` vs
  `EVAL_SAFE`; mean accepted guess risk (mean `best_prob` on guess moves). The
  opening move (`eval == EVAL_START`) is counted separately and excluded from
  guess stats and calibration (it is a forced first click, not a deduced guess).
- **Guess calibration curve** — for each *chosen guess*, sample
  `(predicted P(mine) = best_prob, actually_mine ∈ {0,1})`: a survived guess is
  known-safe (0), a fatal guess known-mine (1). Bucketed into ~20 bins over [0,1];
  report predicted-mean vs empirical-rate per bin. Drift from the diagonal is the
  proxy for fallback damage in dense midgames.
- **`deaths_on_forced_safe`** — count of losses where the fatal cell had
  `forced_safe == true` (or `best_prob < EPS`). Must be 0; nonzero ⇒ engine
  correctness bug. The harness doubles as a correctness gate.
- **Progress at death** — mean revealed-fraction
  (`revealed_count / (cells - mines)`) when a game is lost.
- **Perf** — wall time, games/sec, mean & max µs per `solver_analyze`.

## 8. CLI & output

```
minesweeper_bench [--difficulty beginner|intermediate|expert]
                  [--games N] [--seed S] [--policy baseline]
                  [--threads T] [--quiet]
                  [--width W --height H --mines M]   # custom, within 30×24
```

- Defaults: `--difficulty expert`, `--games 1000000`, `--seed 1`,
  `--policy baseline`, `--threads <hw concurrency>`.
- Output: a human-readable summary table to stdout (winrate ± CI, guess stats,
  calibration table, progress-at-death, perf). No CSV in Phase 1.
- ⚠️ The 1M default is a long run (≈ tens of minutes to a few hours, depending on
  cores and per-analyze cost). Scale down with `--games` for quick smoke runs.

## 9. Testing

`bench_test` (gtest, links `solver_lib` + `game_core`):
- **Determinism golden** — Beginner, fixed seed, small game count; assert exact
  win/loss counts (regression guard on the play loop + policy).
- **Thread-invariance** — same `(seed, games)` yields identical aggregate metrics
  at `--threads 1` and `--threads 4`.
- **Correctness invariant** — `deaths_on_forced_safe == 0` over a batch.
- **Wilson CI math** — unit test against known values.

Wired into `tests/CMakeLists.txt` via `gtest_discover_tests`.

## 10. Statistical guidance

- **Common random numbers (paired comparison):** because each board is fixed by
  its seed, two policies run on the same `(base_seed, games)` see identical boards.
  Compare per-board outcomes for large variance reduction — small strategy deltas
  become detectable. (Phase 1 ships one policy; this guidance steers the strategy
  phase.)
- Rough Expert Wilson half-width near p≈0.35: ±2.1pp @ 2k games, ±1.3pp @ 5k,
  ±0.9pp @ 10k, ~±0.05pp @ 1M.
- Realistic expectation: strong Expert solvers reach ~40%; this run reveals where
  min-prob selection + fallback approximations actually land.

## 11. Non-goals (Phase 1)

No engine edits; no policies beyond baseline; no 100×100 (Phase 2); no flagging
logic (irrelevant to winning — win = all non-mine cells revealed); no CSV; no GUI.

## 12. Open questions

None outstanding. (100×100 cap-raise and the specific strategy improvements are
deferred to their own brainstorm/spec cycles, informed by this harness's output.)
