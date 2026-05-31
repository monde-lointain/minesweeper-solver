# Smarter Guess-Selection Policy — Design

Date: 2026-05-31
Goal: raise Expert winrate by replacing the engine's min-prob guess with a
lookahead policy that maximizes P(eventually win), not just immediate survival.
Builds on the Phase-1 harness (`minesweeper_bench`) and its baseline report
(`2026-05-31-solver-benchmark-baseline.md`).

## Constraints (locked)

- **No compute cap** — pursue the strongest policy; iterate at small game counts.
- **No hidden information** — the policy may use only what a real player sees
  (revealed numbers, geometry, total `mines`). Never read `cell.mine` for a
  covered cell; never use RNG/seed state. Any lookahead must reason over mine
  layouts *consistent with the visible board*, drawn from the engine's model.
- **Engine extensible additively** — `engine.h` may gain new functions; existing
  `solver_analyze` behavior is unchanged (golden + exact-oracle tests guard it).
- **Orthodox C++** throughout (POD, plain enums, pointers, C headers, fixed
  buffers). Existing `std::thread`/`std::atomic` carve-out in the runner only.
- **Bench-only deployment** for now. App overlay keeps the engine min-prob pick;
  the sampler is written general-purpose so a later task can upgrade the overlay.

## The decision problem

Only `eval == EVAL_GUESS` matters (no proven-safe cell). Baseline picks
`argmin P(mine)`, ignoring two things that decide Expert games:

1. **Progress if safe** — a guess that cascades / unlocks deductions cuts future
   guesses.
2. **Future guess quality** — some low-risk guesses still strand you in 50/50s.

Winning more Expert games means optimizing P(win), not immediate survival.

## Architecture

Policy seam stays the integration point; `policy.cc` becomes a thin dispatcher.

| Unit | Role |
|---|---|
| `src/bench/policy.cc` | dispatch on `PolicyId` (signature unchanged) |
| `src/bench/policy_heuristic.{h,cc}` | Stage 1 scoring + progress proxies; also the rollout inner policy |
| `src/bench/policy_rollout.{h,cc}` | Stage 3 Monte-Carlo lookahead driver |
| `include/solver/engine.h`, `src/engine/engine.cc` | Stage 2 sampler (additive) |

New enum cases: `POLICY_HEURISTIC`, `POLICY_ROLLOUT`. `POLICY_BASELINE` stays —
every comparison is against it.

**Dispatch layering** (cost only where needed): `EVAL_SAFE` → take a forced-safe
cell; `EVAL_START` → Stage 1 opening; `EVAL_GUESS` → the stage's guess logic.
Never reveal a `forced_mine` cell. Expert guesses are ~3.6% of moves, so the
expensive path is rare.

**Determinism / threading:** sampler takes an explicit `uint64_t* rng_state`
(no globals), seeded per decision from (game seed, move index). All decisions
stay a pure function of the game seed → harness thread-count invariance holds.

## Stage 1 — `POLICY_HEURISTIC` (progress-aware min-prob)

Pure policy-layer; no engine change. Also serves as Stage 3's fast inner policy
(input `Board*`+`Analysis*`, output `Move`, no allocation).

- **Primary key:** minimize `p_c` (unchanged from baseline).
- **Tie-break = expected progress**, over cells within a tolerance `band` of the
  min, from visible info + marginals:
  - **Cascade likelihood** `P(c==0) ≈ Π_{n ∈ covered neighbors(c)} (1 − p_n)`
    (revealed neighbors are known-safe → factor 1; interior cell → all 8 covered).
  - **Opening potential**: covered-neighbor count (proxy for cascade reach).
  - **Fresh-info bonus**: prefer an interior reveal over an equal-risk frontier
    reveal — drops a new number into blank territory.
  - Combined into one tunable score (weights are calibrated constants).
- **`band` default 0** — progress breaks only *exact* ties (zero added risk).
  `band` is a knob; deliberate risk/progress trades are Stage 3's job.
- **Opening move** (`EVAL_START`, today hardcoded `(0,0)`): first click is safe
  but can be a number; a corner (3 neighbors) maximizes `P(first==0 → cascade)`.
  `(0,0)` is already a corner. A/B corner vs center vs one-off-corner via the
  harness; keep the winner. Low priority.

## Stage 2 — `solver_sample_config()` (consistent-world sampler)

Additive engine extension. Lives in `engine.cc` (reads `SolverScratch`
internals: `res.shat/mhat/fallback/fb_p`, components, combine DP).

```c
/* Draw one mine layout consistent with the visible board into out_mine
 * (len w*h; 1=mine). Call AFTER solver_analyze(b,_,s) on the same board+scratch
 * (reuses its components + combine DP; analyze cost amortized over many draws).
 * rng_state is caller-owned in/out PRNG state (no globals). Exact components
 * satisfy every visible number and the global mine total; over-budget fallback
 * components are sampled per-var-marginal (matching the engine's own fallback
 * approximation). Returns 0 on success, -1 if unanalyzable. */
int solver_sample_config(const struct Board* b, struct SolverScratch* s,
                         uint64_t* rng_state, unsigned char* out_mine);
```

**Algorithm — three steps, reusing engine internals:**

1. **Mine-count split** across {exact components, fallback bundle, interior}
   conditioned on total `== b->mines`, by backward sampling through the existing
   convolution DP (`prefix`/`suffix`/`fbdist`). Cheap, exact; enforces the global
   mine budget.
2. **Per exact component**, given drawn count `k_c`, draw a uniform concrete
   assignment among its `k_c`-mine solutions via **reservoir sampling over one
   enumeration pass** (reuses the component backtrack; reservoir-keeps one
   solution hitting the target count). Cost center; a batched draw-N variant can
   amortize it if profiling demands.
3. **Fallback components**: distribute count, sample each var `Bernoulli(fb_p)`
   (approximate, documented). **Interior**: uniform random subset of size `r`.

Union → full layout.

**Internal refactor:** extract the per-component `EnumScratch` setup (currently
inline in `solver_analyze`) into a shared static helper so analyze and the
sampler build a component identically. Behavior-preserving; golden test guards.

**Tests (correctness-critical unit):**

- **Marginal recovery** (lynchpin): over ~100k draws, empirical per-cell mine
  frequency matches `solver_analyze`'s `mine_prob` within sampling tolerance.
  Proves the sampler draws from exactly the marginals' distribution.
- **Constraint satisfaction**: every draw satisfies every revealed number and the
  global mine count — 100% for exact components; *measured + documented*
  violation rate for fallback components.
- **Determinism**: same `rng_state` → identical draws; thread-invariant.
- **Degenerate**: fully-determined frontier → unique layout; all-interior →
  uniform subsets.

## Stage 3 — `POLICY_ROLLOUT` (Monte-Carlo lookahead)

Per guess decision:

1. **Candidate shortlist** — covered cells with `p_c ≤ p_min + MARGIN` (capped at
   `K_max`), plus one interior representative.
2. **Draw N consistent worlds once** (Stage 2), shared across all candidates —
   common random numbers → low-variance candidate *differences* at modest N.
3. **Score by rollout**: in each world `w`, reveal `c`, then play the Stage 1
   inner policy forward to terminal. `Score(c) = mean win over N worlds`. Worlds
   where `c` is a mine contribute losses, so `p_c` is absorbed — no separate risk
   term.
4. **Pick `argmax Score`**; near-tie → fall back to Stage 1 heuristic pick.

**Soundness:** `Score(c) ≈ P(win | play c, then inner policy)`; argmax is one step
of policy improvement over the inner policy → Stage 3 ≥ Stage 1 in expectation,
modulo sampling noise.

**Rollout mechanics:** replay through the real `game_reveal`/flood on a
hypothetical `Board` (current visible state + world `w`'s mines) — reuses actual
game logic for cascades and win detection.

**Two scratches per worker:** outer decision uses `s` (analyze + draw all N
worlds up front into a buffer); rollouts drive the inner policy's
`solver_analyze` on a separate `s2`, so inner analyses never clobber the outer
decision's components.

**Knobs (config-exposed, tuned vs harness):** `N` (worlds), `MARGIN`/`K_max`
(candidate breadth), `depth` (default ∞ = full rollout for fidelity; depth cap +
progress-fraction leaf value is the perf escape hatch).

**Determinism:** the N worlds are a pure function of (game seed, move index) →
thread-count invariant.

## Validation & success criteria

**Paired A/B** — new harness mode `--compare baseline,rollout`: per seed, play the
same board under both policies in one worker, tally the 2×2 discordance table
(both win / both lose / A-only / B-only) → winrate delta with a paired interval
(McNemar). Tighter than two independent Wilson intervals; needed to detect a
few-point gain at small game counts. Counts merge commutatively → thread-invariant.

**Gate per stage:**

- **Expert:** winrate(stage) > winrate(baseline), paired-significant
  (McNemar p < 0.05 / delta CI excludes 0).
- **Non-regression:** Beginner & Intermediate not significantly worse (paired).
- **Correctness:** `deaths_on_forced_safe == 0` for every policy.
- Each stage clears its gate before the next is built.

**Metrics additions** (`metrics.h`, additive counts/sums — commutative merge):
rollout cost (mean candidates, worlds, inner analyses, ns); divergence-from-
baseline-pick rate; discordance counts for compare mode.

**Tuning protocol:** sweep `N / MARGIN / K_max / depth` on Expert at a few
thousand paired games; pick the knee; lock defaults; one larger confirmation run.
Record chosen values + achieved winrate in the baseline report.

**Build/correctness gates (unchanged rigor):** gcc `-Werror` and clang-22 +
Orthodoxy on all new files; `engine_test` exact oracle byte-identical (sampler
additive; `solver_analyze` untouched); golden test guards the enum-setup refactor.

## Risks / re-surface triggers

- Sampler **marginal-recovery test fails** → lynchpin bug; stop and fix.
- Rollouts too slow for significance at feasible N → depth cap + leaf value,
  narrow `K`, add variance reduction; if still stuck, re-surface.
- **Correct sampler but no significant Expert gain** → headroom isn't in guess
  *ordering* (it's accuracy / "full B"); re-surface rather than grind.
- Beginner/Intermediate regression → `MARGIN` too aggressive; investigate.

## Staging summary

1. Stage 1 `POLICY_HEURISTIC` — pure policy-layer; measured vs baseline.
2. Stage 2 `solver_sample_config()` — isolated engine extension; standalone tests.
3. Stage 3 `POLICY_ROLLOUT` — composes 1 + 2; tune knobs; gate on Expert winrate.

Each stage = its own commit + measured winrate.

## Out of scope

- App overlay integration (sampler kept general-purpose for a later task).
- "Full B" engine accuracy / fallback `fb_p` marginals.
- Phase-2 100×100 / 2440-mine board-cap raise.
