# Solver Benchmark Harness — Parallel Execution Plan

Date: 2026-05-31
Source spec: `2026-05-31-solver-benchmark-harness-design.md` (approved)
Convention: alpha-numeric streams, partition-by-file (R8).

## Dispatch prerequisites (mandatory)

- `superpowers:using-git-worktrees` — every parallel stream runs in its own
  worktree off the post-A commit. No shared working tree.
- `superpowers:dispatching-parallel-agents` — governs the B-stream fan-out.
- Branch base: `bench-harness`.

## Target build graph (created by Stream A, consumed by all)

- `game_core` (STATIC) = sibling `game.cc` + `util.cc` — SDL-free, warnings-only,
  no Orthodoxy (reused code).
- `bench_lib` (STATIC) = `bench/policy.cc bench/metrics.cc bench/runner.cc
  bench/args.cc` → links `solver_lib` + `game_core` + `Threads::Threads`.
  Orthodoxy + `-Werror`.
- `minesweeper_bench` (EXE) = `bench/main.cc` → links `bench_lib`.
- Test exes (each links `bench_lib` + `gtest_main`): `bench_policy_test`,
  `bench_metrics_test`, `bench_runner_test`, `bench_cli_test`, `bench_test`.

## Header contract (created by Stream A, frozen at A-barrier)

- `policy.h` — `struct Move`, `enum PolicyId`, `policy_select(...)`. incl engine.h, game.h.
- `metrics.h` — `struct Metrics`, `metrics_merge/wilson/calib_add/print`.
- `runner.h` — `struct BenchConfig`, `bench_run(const BenchConfig*, struct Metrics*)`. incl metrics.h, policy.h.
- `args.h` — `bench_parse_args(int,char**,struct BenchConfig*,const char**)`. incl runner.h.

## Stream dependency graph

```
A (contracts + build graph + functional stubs)   [BARRIER]
├─ B.1 policy.cc        ┐
├─ B.2 metrics.cc       │ fan-out, mutually file-disjoint
├─ B.3 runner.cc        │
└─ B.4 args.cc+main.cc  ┘
        ↓ (all four merged)
C integration + baseline                          [FAN-IN BARRIER]
```

Merge order: A → {B.1,B.2,B.3,B.4 any order} → C.

---

## Stream A — Contracts & build scaffold

**Goal:** Land the header contracts, build graph, and green functional stubs so B-streams compile and test in isolation.

**Scope:** All interfaces + CMake wiring + minimal-functional stub bodies. No real behavior.

**Files Owned:**
- `src/bench/policy.h`, `src/bench/metrics.h`, `src/bench/runner.h`, `src/bench/args.h`
- `src/CMakeLists.txt`, `tests/CMakeLists.txt`
- Stub `.cc` (created here, ownership transfers post-barrier): `src/bench/policy.cc`,
  `src/bench/metrics.cc`, `src/bench/runner.cc`, `src/bench/args.cc`, `src/bench/main.cc`
- Stub tests (ownership transfers): `tests/bench_policy_test.cc`,
  `tests/bench_metrics_test.cc`, `tests/bench_runner_test.cc`,
  `tests/bench_cli_test.cc`, `tests/bench_test.cc`
- `CMakeLists.txt` root (add `find_package(Threads REQUIRED)` only)

**Files Forbidden:** engine/overlay/app sources; `include/solver/*`; sibling tree; `tests/{engine,overlay,golden}_test.cc`.

**Dependencies:** None — root.

**Parallelism Classification:** Barrier-Gated (gate for all B).

**Stub behavior (must be functional, not no-ops):**
- policy stub → first covered cell (lets games terminate).
- metrics stub → tally games/wins/losses only.
- runner stub → single-threaded loop; ignores `threads`.
- args stub → fill defaults, ignore argv.
- main stub → run defaults, print one summary line.

**Verification:** `make build` green; `make format` + `make tidy` clean (Orthodoxy passes on bench_lib/exe); `minesweeper_bench --help` exits 0; `make test` green (all stub tests pass).

**Deliverables:** 1 commit on `bench-harness`; configured build with all bench targets present and green.

**Failure Isolation:** Root — failure blocks every downstream stream. No B dispatch until A merges green.

**Re-Surface Triggers:** header signature cannot satisfy a downstream need discovered mid-impl (contract drift); `Threads::Threads`/std::thread unavailable in toolchain; Orthodoxy rejects an unavoidable std::thread/atomic construct in a header.

---

## Stream B.1 — Policy

**Goal:** Implement `POLICY_BASELINE` reproducing the engine's min-prob pick.

**Scope:** `policy_select` body only.

**Files Owned:** `src/bench/policy.cc`, `tests/bench_policy_test.cc`.

**Files Forbidden:** all headers (A-frozen), every other `.cc`, all CMake.

**Dependencies:** A.

**Parallelism Classification:** Fully Parallel (file-disjoint from B.2/B.3/B.4).

**Verification:** `bench_policy_test`: baseline returns `analysis.best_x/best_y`; returns -1 when no covered cell; never selects a revealed cell. `make tidy`/`format` clean.

**Deliverables:** 1 commit; passing `bench_policy_test`.

**Failure Isolation:** Blocks only C (and runtime correctness). B.2/B.3/B.4 unaffected.

**Re-Surface Triggers:** baseline cannot be reproduced from `Analysis` fields alone (would force an engine change — out of Phase-1 scope).

---

## Stream B.2 — Metrics

**Goal:** Implement Wilson CI, calibration buckets, merge, and summary formatting.

**Scope:** all `metrics.*` function bodies; stdout summary table layout.

**Files Owned:** `src/bench/metrics.cc`, `tests/bench_metrics_test.cc`.

**Files Forbidden:** all headers, every other `.cc`, all CMake.

**Dependencies:** A.

**Parallelism Classification:** Fully Parallel.

**Verification:** `bench_metrics_test`: Wilson bounds vs known values; calibration bin assignment + per-bin predicted-vs-empirical; `metrics_merge` commutative & associative (`merge(a,b)==merge(b,a)`); `max`-reduced perf fields. `make tidy`/`format` clean.

**Deliverables:** 1 commit; passing `bench_metrics_test`.

**Failure Isolation:** Blocks only C. Others unaffected.

**Re-Surface Triggers:** calibration sampling needs per-cell ground truth beyond the chosen cell (would change the metric contract).

---

## Stream B.3 — Runner

**Goal:** Implement the play loop + `std::thread`/`std::atomic` fan-out + per-worker metric accumulation + merge.

**Scope:** `bench_run`; worker pool; atomic next-game counter; per-worker `SolverScratch` + `Metrics`; deterministic merge.

**Files Owned:** `src/bench/runner.cc`, `tests/bench_runner_test.cc`.

**Files Forbidden:** all headers, every other `.cc`, all CMake.

**Dependencies:** A. (Compiles/links against A's policy+metrics stubs; real behavior validated in C.)

**Parallelism Classification:** Fully Parallel (file-disjoint). Runtime behavioral validation deferred to C fan-in.

**Verification:** `bench_runner_test` against stubs: harness thread-invariance (identical aggregate counts at `--threads 1` vs `4`); per-seed determinism (same `(seed,games)` → same outcomes); games-run count == requested; no data race under TSan-equivalent reasoning (no shared mutable non-atomic state). `make tidy`/`format` clean.

**Deliverables:** 1 commit; passing `bench_runner_test`.

**Failure Isolation:** Blocks only C. Others unaffected.

**Re-Surface Triggers:** observed non-determinism across thread counts; per-worker scratch memory pressure infeasible at default thread count; `Analysis` lacks a field the loop needs for guess/forced-safe classification.

---

## Stream B.4 — CLI

**Goal:** Implement arg parsing (`args.cc`) + thin `main.cc` wiring parse→run→print.

**Scope:** flag parse/validate (difficulty presets, custom dims clamp to 30×24, games/seed/threads/policy/quiet); `main` dispatch.

**Files Owned:** `src/bench/args.cc`, `src/bench/main.cc`, `tests/bench_cli_test.cc`.

**Files Forbidden:** all headers, every other `.cc`, all CMake.

**Dependencies:** A.

**Parallelism Classification:** Fully Parallel.

**Verification:** `bench_cli_test`: preset→dims mapping; custom-dim clamp + rejection of out-of-range; defaults (`expert`, `games=1000000`, `seed=1`, `threads=hw`); bad-flag → nonzero + message. `make tidy`/`format` clean.

**Deliverables:** 1 commit; passing `bench_cli_test`; `minesweeper_bench --help` usage text.

**Failure Isolation:** Blocks only C. Others unaffected.

**Re-Surface Triggers:** a needed run parameter is absent from `BenchConfig` (contract gap → escalate to A owner).

---

## Stream C — Integration & baseline

**Goal:** Replace integration stub with full end-to-end tests against real modules; capture baseline numbers.

**Scope:** behavioral + correctness validation across the assembled `bench_lib`; baseline smoke.

**Files Owned:** `tests/bench_test.cc`, `docs/superpowers/specs/2026-05-31-solver-benchmark-baseline.md`.

**Files Forbidden:** all `src/bench/*` (B-owned), all headers/CMake (A-owned).

**Dependencies:** B.1, B.2, B.3, B.4 (all merged). Fan-In.

**Parallelism Classification:** Fan-In (barrier).

**Verification:** `bench_test`: determinism golden (Beginner fixed seed, exact win/loss counts); thread-invariance with REAL modules (`--threads 1` vs `4` identical aggregate); `deaths_on_forced_safe == 0` over a batch; end-to-end calibration sane (monotone, in-range). Then baseline smoke: Expert + Intermediate at reduced `--games` (e.g. 5k) — record winrate ± CI, forced-guess rate, calibration drift, perf. (Full 1M run is user-initiated, not part of CI.)

**Deliverables:** 1 commit; passing `bench_test`; baseline report doc with the smoke numbers; PR `bench-harness` → `main`.

**Failure Isolation:** Terminal — failure blocks only final merge/sign-off.

**Re-Surface Triggers:** `deaths_on_forced_safe > 0` (engine correctness defect — halt, do not merge); thread-variance detected end-to-end; baseline winrate implausible (e.g. Beginner < 50% or Expert ≈ 0%) indicating a play-loop/policy bug; per-analyze perf so high that 1M is infeasible even threaded (feeds back to a perf-strategy decision).

---

## Global re-surface triggers

- Any shared-file contention not anticipated here → stop, re-partition.
- Engine/overlay/app or frozen-contract edit becomes necessary → exits Phase-1 scope; escalate.
- Orthodoxy/`-Werror` rejects a construct with no compliant rewrite → escalate (std::thread/atomic are pre-sanctioned; others are not).
