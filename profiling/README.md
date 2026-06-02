# profiling/

callgrind/kcachegrind profiling of the solver's pathological workload.

## What gets profiled

`src/profiler/main.cc` (`minesweeper_profiler`) — a bounded replay of the run
that motivated this: **100×100, 2500 mines, seed 7, info-gain policy**, the exact
bench `play_one` loop truncated to a move cap (default 2000, `argv[1]` to override;
`<=0` = full game). The cap only truncates, so the profiled path is faithful; the
workload is deterministic, so profiles are comparable across optimisation passes.
The default 2000 is tuned to reach the reduction-heavy regime that dominates
per-move cost while keeping the callgrind run ≈9 min (the full game is ≈1 h).

## Run it

```sh
make profile          # build-profile/ (Release + -g), then callgrind the runner
make profile-analyze  # callgrind_annotate -> reports/
kcachegrind profiling/callgrind.out.minesweeper   # interactive
```

`make profile` targets ≈9 min of callgrind wall-time (≈72× native at the default
2000-move cap, `--dump-instr=no` for function/line-level annotation); tune via the
runner's move cap if needed (`build-profile/src/minesweeper_profiler <max_moves>`).

## Layout (for `/optimization-report`)

- `callgrind.out.minesweeper.<ts>` — raw callgrind output; `callgrind.out.minesweeper`
  symlinks the latest.
- `reports/minesweeper_annotate_<ts>.txt` — per-source annotation (instruction counts).
- `reports/minesweeper_summary_<ts>.txt` — `events:`/`summary:` + hottest functions.
- `scripts/analyze_profile.sh` — regenerates the reports from the latest raw output.

Raw outputs and `reports/` are gitignored; only the scaffold is tracked.
