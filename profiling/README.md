# profiling/

callgrind/kcachegrind profiling of the solver's pathological workload.

## What gets profiled

`src/profiler/main.cc` (`minesweeper_profiler`) — a bounded replay of the run
that motivated this: **100×100, 2500 mines, seed 7, info-gain policy**, the exact
bench `play_one` loop truncated to a move cap (default 500, `argv[1]` to override;
`<=0` = full game). The cap only truncates, so the profiled path is faithful; the
workload is deterministic, so profiles are comparable across optimisation passes.

## Run it

```sh
make profile          # build-profile/ (Release + -g), then callgrind the runner
make profile-analyze  # callgrind_annotate -> reports/
kcachegrind profiling/callgrind.out.minesweeper   # interactive
```

`make profile` targets ~3 min of callgrind wall-time; tune via the runner's move
cap if needed (`build-profile/src/minesweeper_profiler <max_moves>`).

## Layout (for `/optimization-report`)

- `callgrind.out.minesweeper.<ts>` — raw callgrind output; `callgrind.out.minesweeper`
  symlinks the latest.
- `reports/minesweeper_annotate_<ts>.txt` — per-source annotation (instruction counts).
- `reports/minesweeper_summary_<ts>.txt` — `events:`/`summary:` + hottest functions.
- `scripts/analyze_profile.sh` — regenerates the reports from the latest raw output.

Raw outputs and `reports/` are gitignored; only the scaffold is tracked.
