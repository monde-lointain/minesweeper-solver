---
name: rescuing-ci-failures
description: Use when a GitHub Actions ci.yml run is red on the current branch (jobs build-linux / build-macos / build-windows) and the goal is to pull failed-job logs, triage, fix, verify, push, and iterate until green. Especially the macOS (AppleClang) and Windows (MSVC) legs, which CANNOT be built from this Linux host, so their fixes are blind edits the next CI run validates. Bounded loop: per-job error-signature dedup, iteration cap, local-verify-first for Linux, category gate. Lean inline state (no helper scripts, no committed state).
---

# Rescuing CI Failures

When the `ci.yml` GitHub Actions run is red on the current branch, this skill
drives a bounded loop: resolve the run for the current HEAD → pull failed-job
logs → triage one job → dispatch a fix subagent → verify (locally for Linux,
by-reasoning for macOS/Windows) → push → wait for the next run → repeat. The
loop is hard-bounded by an iteration cap, per-job same-signature dedup, and a
category gate, so it cannot run away.

This is the sibling adaptation of the `minesweeper` project's
`rescuing-ci-failures` skill: same algorithm and stop conditions, tuned to
**this** project's three jobs and its overlay-on-fetched-upstream build. **No
helper script and no committed state** — iteration state lives inline (a
TodoWrite list + an uncommitted `/tmp/ci-rescue-<branch>.json` scratch file).

**The defining constraint:** the Linux job (`build-linux`) reproduces locally on
this host; `build-macos` and `build-windows` do **not**. So Linux fixes are
verified locally before the slow GHA round-trip, but mac/win fixes are reasoned
from the log, confirmed with the user, and proven only by the next CI run.

## When to invoke

- A push comes back red on GitHub Actions.
- The user says "fix CI" / "make CI green" / "the build's broken".
- The `build-macos` / `build-windows` jobs red on their first runs (the warning
  posture is real — `/W4 /WX` on MSVC, `-Werror` on AppleClang — so genuine
  cross-compiler warnings in our own code are the expected first-run failures;
  see Known first-run failures).
- `/ci-rescue` slash command is the standard entry point.

## Preflight

```bash
gh auth status >/dev/null 2>&1 || { echo "gh not authed"; exit 12; }
git rev-parse --is-inside-work-tree >/dev/null || exit 12
branch="$(git rev-parse --abbrev-ref HEAD)"
```

Two **baseline-match preconditions** — the fix must be built against the code the
failing log actually describes:

1. **Clean tree.** `git status --porcelain` must be empty. Otherwise the
   subagent's commit would mix with the user's WIP — halt and ask the user to
   stash/commit first.
2. **HEAD matches the run.** Only triage a run whose `headSha == HEAD` (see run
   resolution). A run behind HEAD describes stale code.

**Branch == main is a warn, not a halt** (this project has historically committed
directly to main; main is unprotected). On main, AskUserQuestion to confirm
before proceeding — CI red on main is usually an incident, not a routine rescue.
Otherwise push target is the existing upstream (`origin/<branch>`); if the branch
has no upstream yet, push with `-u` (confirm first).

## Inline state

No helper script. Maintain a TodoWrite list for the loop, plus an uncommitted
scratch file `/tmp/ci-rescue-<branch>.json`:

```json
{
  "status": "running",
  "branch": "<branch>",
  "max_iterations": 6,
  "auto_push": false,
  "iteration_count": 0,
  "last_signature_by_job": { "build-linux": "sha256:…" },
  "attempted_fixes": [
    { "iteration": 1, "job": "build-windows", "category": "compiler-error",
      "summary": "…", "commit_sha": "…", "signature": "sha256:…" }
  ]
}
```

- `status` is an **already-running guard**: a second `/ci-rescue` invocation that
  finds `status == "running"` for this branch **resumes** rather than starting a
  parallel loop.
- `last_signature_by_job` is keyed **per job** — dedup is per job (see below).
- **Durability is bounded:** `/tmp` is the sole dedup memory across a context
  compaction; a reboot clears it and the loop resets. Accepted trade-off for
  keeping no committed state.
- **On green:** set `status: idle` (or delete the file) so a later invocation
  starts fresh.

## Algorithm

1. **Read state.** If `/tmp/ci-rescue-<branch>.json` `status == "running"` and the
   branch matches, resume; else init (`max_iterations=6`, `auto_push=false` unless
   overridden by `/ci-rescue` args).

2. **Resolve the run.** Filter by workflow so an unrelated workflow can't be
   grabbed:

   ```bash
   gh run list --branch "$branch" --workflow ci.yml --limit 1 \
     --json databaseId,status,conclusion,headSha,workflowName
   ```

   **Staleness guard.** The newest run can be *behind* local HEAD. If
   `run.headSha != HEAD`, the failure does not reflect current code — do NOT
   triage it:
   - HEAD unpushed → push it (with confirm) and wait for its run.
   - A run for HEAD merely in-flight → wait.

   Only triage a run whose `headSha == HEAD`.

3. **Branch on `conclusion`:**
   - `success` → set `status: idle`; done (exit 0).
   - `failure` → step 4.
   - `cancelled` → ci.yml sets **no** `concurrency`/`cancel-in-progress`, so a
     cancel is NOT a routine side effect of consecutive pushes. If a newer run
     exists for a later head SHA, re-resolve to the newest; otherwise treat as a
     genuine cancel / runner abort and escalate `unactionable`.
   - `null` with `status` `in_progress`/`queued`/`pending` → **wait** (see Waiting).

4. **Pull failed-job logs via the REST API.** `gh run view --log[-failed]` is
   unreliable (exits 0 with empty output for multi-job/older runs). Use the
   per-job API path:

   ```bash
   repo="$(gh repo view --json nameWithOwner --jq .nameWithOwner)"
   run_id="<resolved id>"
   out="/tmp/ci-rescue-$run_id.log"; : > "$out"

   mapfile -t failed_jobs < <(gh run view "$run_id" --json jobs \
     --jq '.jobs[] | select(.conclusion=="failure") | "\(.databaseId)\t\(.name)"')

   if [[ ${#failed_jobs[@]} -eq 0 ]]; then
     echo "conclusion=failure but no failed jobs → workflow-level failure"; exit 12
   fi

   for line in "${failed_jobs[@]}"; do
     job_id="${line%%$'\t'*}"; name="${line#*$'\t'}"
     printf '\n========== JOB %s — %s ==========\n\n' "$job_id" "$name" >> "$out"
     gh api "repos/$repo/actions/jobs/$job_id/logs" >> "$out" 2>&1
   done
   ```

   API logs are timestamp-prefixed (`2026-05-16T13:54:37.4679598Z ##[error]…`);
   downstream regexes must tolerate the prefix (the signature step strips it).

5. **Select ONE job** (one job per iteration — see Why one job). When several jobs
   fail, pick by deterministic order, locally-reproducible first so the fix can be
   verified fast: `build-linux` → `build-windows` → `build-macos`. Triage **that
   job's log section only**.

6. **Triage** the selected job into one category + compute its error signature.
   See Triage and Error signature.

7. **Dedup check** (per job): if `signature == last_signature_by_job[job]` →
   escalate `same-error-twice`; AskUserQuestion ("the prior fix didn't change this
   job's failure — review needed").

8. **Cap check:** if `iteration_count >= max_iterations` → escalate
   `iteration-cap`.

9. **Category gate:**
   - `transient` → `gh run rerun --failed "$run_id"` once (no code change, no
     iteration consumed); re-resolve at step 2. If it recurs identically, demote
     to `unactionable` and escalate.
   - `unactionable` → escalate with the matched pattern.
   - `compiler-error` / `test-failure` / `lint` → step 10.

10. **Dispatch the fix subagent** (see Subagent dispatch).

11. **Verify the subagent committed:**
    ```bash
    head_after="$(git rev-parse HEAD)"
    [[ "$head_after" != "$head_before" ]] || { echo "subagent did not commit"; exit 13; }
    git diff --quiet HEAD~1 HEAD && { echo "empty commit"; exit 13; }
    ```

12. **Verify locally (before push) — Linux only.** If the job is `build-linux`, run
    the mapped command and require green (see Local verification). `build-macos` /
    `build-windows` are NOT reproducible from this Linux host — skip local verify
    and rely on the next CI run.

13. **Confirm push.** If `auto_push == true` AND the job is locally verified
    (`build-linux` only) → push. Otherwise (**always** for mac/win) →
    AskUserQuestion with the diff summary; on rejection, escalate `divr`.

14. **Push** `git push` (or `git push -u origin "$branch"` if no upstream);
    capture the new `head_sha`.

15. **Record iteration** into the scratch file: append to `attempted_fixes`, set
    `last_signature_by_job[job] = signature`, bump `iteration_count`.

16. **Wait for the new run to appear**, then loop to step 2.

## Why one job per iteration

Aligns with `[[superpowers:systematic-debugging]]` — one hypothesis at a time.
The three jobs are independent (not a matrix), so multiple can red in one run;
each is consumed by a separate iteration. Fixing one job and exposing a
*different* failure in another job is expected progress, not a regression.

## Waiting

Every job builds SDL3 / SDL3_mixer / ImGui / gtest from source via FetchContent
*and* fetches the pinned minesweeper upstream sources (the game this project
overlays), so all three jobs are slow. ETA rules of thumb:

- `build-linux` (Linux) ≈ 10–20 min — also installs clang-22 via `llvm.sh` and the
  SDL X11/Wayland apt deps. Do NOT block; `ScheduleWakeup` (≥270s) re-entering
  `/ci-rescue`, which reads `status: running` and resumes.
- `build-macos` / `build-windows` ≈ 5–15 min, vary with runner availability →
  `ScheduleWakeup`, or `gh run watch "$run_id" --exit-status` if foregrounding.

## Triage

Classify the **selected job** by the **failed STEP name first**, then by pattern.
The REST API log delineates steps (`##[group]Run …` headers). Keying on the step
disambiguates look-alike text.

| Failed step | Likely category |
|---|---|
| `Install toolchain and SDL build dependencies` (Linux) | `transient` (apt / `llvm.sh` / network) |
| `Configure` | `compiler-error` (CMake configure error, missing dep, generator) — but a failure *fetching the pinned minesweeper upstream commit or any FetchContent dep* is `transient` (network) |
| `Build` | `compiler-error` (warnings-as-errors, link errors) |
| `Test` | `test-failure` |
| `clang-tidy` (Linux only) | `lint` |

Within the step, apply patterns (first match wins). API lines carry a timestamp
prefix; patterns are substrings/regexes that tolerate it.

| Pattern | Category |
|---|---|
| `error C[0-9]{4}` / `warning C[0-9]{4}` (MSVC — `/WX` makes warnings fatal: C4267 `size_t`→`int`, C4244, C4100) | `compiler-error` |
| `error:` / `fatal error:` (clang/AppleClang; `-Werror`) | `compiler-error` |
| `LNK[0-9]{4}` (esp. `LNK2038` CRT mismatch) / `undefined reference` / `Undefined symbols` / `ld:` | `compiler-error` (link) |
| `\[  FAILED  \]` (gtest) / ctest `Failed` / `tests failed` | `test-failure` |
| clang-tidy `warning:` / `error:` emitted from the `clang-tidy` step | `lint` |
| `apt`/`llvm.sh`/FetchContent/`git clone` failure, `timed out`, `429`, `no space left`, `connection refused`, runner quota | `transient` |
| anything unclassifiable | `unactionable` |

Default → `unactionable`.

**Solver-specific note — test exes are SDL-free.** Unlike the sibling
`minesweeper`, **none of this project's test executables link SDL.** Every test
links `solver_lib` or `bench_lib` + `gtest_main` (see `tests/CMakeLists.txt`);
neither pulls in SDL3. The SDL-linking targets (`minesweeper_solver`,
`minesweeper_reuse`, `imgui`) are built but never *run* in CI. So the Windows
runtime-DLL failure class (`SDL3.dll` not found / `0xc0000135` / "code execution
cannot proceed") that the sibling skill warns about **does not apply here** — if
it ever appears, a new test target started linking an SDL target and the fix is
to keep tests SDL-free (or copy the runtime DLLs next to that exe).

**Not CI failure modes here** (do not waste a hypothesis on them):
- **Orthodoxy violations.** The Orthodoxy plugin is NOT installed in CI
  (`cmake/Orthodoxy.cmake` → `orthodoxy_plugin_FOUND` is false), so
  `[orthodoxy::…]` errors never appear in CI. Enforcement is local-only.
- **`make format` / `format-patch`.** Run nowhere in CI; no commit hooks.
  (Note: `clang-tidy` DOES run in CI and CAN fail — that is the `lint` category.)

## Known first-run failures (macOS / Windows)

`build-macos` and `build-windows` were added without an Apple/Windows host to
verify them. The project already carries the standard cross-platform CMake
accommodations (per-compiler warning flags, `_CRT_SECURE_NO_WARNINGS`,
`gtest_force_shared_crt ON`, `SDL_X11_XTEST OFF`), so the remaining first-run
risks are **genuine compiler warnings in our own code** under the strict posture.
Give the fix subagent these priors:

| Symptom | Job | Standard fix |
|---|---|---|
| `error C4267` / `C4244` / `C4100` (MSVC `/W4 /WX`) | build-windows | Minimal correct fix in our `src/*.cc` (explicit cast / `(void)param` / type fix) — never widen scope, never drop `/W4` or `/WX` |
| `error: …` from `-Werror` (AppleClang stricter than Linux clang-22) | build-macos | Minimal correct fix in the flagged `src/*.cc` |
| `LNK2038` "mismatch detected for 'RuntimeLibrary'" (gtest `/MT` vs project `/MD`) | build-windows | Should already be mitigated by `set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)` in `cmake/Dependencies.cmake` (inside the `BUILD_TESTS` block, before `FetchContent_MakeAvailable(googletest)`). If it still appears, that force isn't taking effect — verify the line is present and runs before gtest is populated. |
| SDL `XTEST`/`libxtst` configure error (Linux) | build-linux | Should already be mitigated by `set(SDL_X11_XTEST OFF)` in `cmake/Dependencies.cmake`. If it recurs, confirm the line precedes `FetchContent_MakeAvailable(SDL3)`. |

These edits touch our `src/*.cc` or `cmake/Dependencies.cmake`, **never the
warning posture** (`/W4` on MSVC / `-Wall -Wextra -Wpedantic` elsewhere via
`SOLVER_WARNINGS`, `COMPILE_WARNING_AS_ERROR` gated on `MINESWEEPER_SOLVER_WERROR`)
— the point of the jobs is to keep warnings fatal. Do NOT "fix" reused sibling
sources (`minesweeper_reuse` / `game_core`); they build warnings-only (not
`-Werror`) so they should not fail the build, and they belong to the upstream
repo.

## Error signature

Deterministic SHA256 over a normalized failure fingerprint, computed over the
**selected job's** log section. Stored **per job** in `last_signature_by_job[job]`.

```bash
# API logs are timestamp-prefixed; strip first so anchored regexes work and
# timestamps don't pollute the fingerprint.
stripped="$(sed -E 's/^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.Z-]+ //' "$job_log")"

failed_markers="$(printf '%s\n' "$stripped" \
  | grep -E '^##\[error\]|\[  FAILED  \]|error:|error C[0-9]{4}|LNK[0-9]{4}|undefined reference' \
  | sort -u)"

fingerprint="$(printf '%s\n' "$stripped" \
  | grep -B5 -A0 -E '^##\[error\]|error:|error C[0-9]{4}|warning C[0-9]{4}|LNK[0-9]{4}|\[  FAILED  \]|undefined reference|ctest.*Failed' \
  | sed -E 's/:[0-9]+:[0-9]+:/:LINE:COL:/g; s/0x[0-9a-fA-F]+/0xHEX/g' \
  | sort -u)"

signature="sha256:$(printf '%s\n%s' "$failed_markers" "$fingerprint" | sha256sum | cut -d' ' -f1)"
```

The `LINE:COL` / `0xHEX` normalizations prevent trivial off-by-one diffs from
defeating dedup. Dedup compares against `last_signature_by_job[job]`: fixing
`build-windows` and exposing a new `build-macos` failure is NOT a hit; re-failing
`build-windows` identically IS.

## Failed-job → local-repro mapping

| Failed job | Local command (verify the fix BEFORE push) |
|---|---|
| `build-linux` (Linux) | `cmake -B build-rescue -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22` → `cmake --build build-rescue -j"$(nproc)"` → `ctest --test-dir build-rescue --output-on-failure`. For a `lint` failure also: `cmake --build build-rescue --target tidy`. |
| `build-windows`, `build-macos` | Not reproducible from this Linux host — read the log, reason, confirm the blind edit with the user, let the next CI run be the test. |

## Local verification

For `build-linux`, run the mapped command; require it green before pushing.
Caveats:

- **Use a dedicated `build-rescue/` dir** (the command above), not the user's
  `build/`. The default `build/` is `Debug` (Makefile default `BUILD_TYPE=Debug`);
  pinning `Release` in it would force a reconfigure and clobber the user's working
  build. (`build-rescue/` is already in `.gitignore` via the `build*` pattern;
  confirm before creating if unsure.)
- **Pin clang-22.** CI configures with `clang-22`/`clang++-22`; pin both in
  `build-rescue/` for fidelity (the Makefile defaults already use clang-22, but
  pin explicitly so the repro matches CI exactly).
- **Local-green is advisory, not authoritative.** A local pass gates the push; if
  CI then reds with the *same* signature, that's the `same-error-twice`
  escalation (likely an env divergence to surface).
- **mac/win cannot be locally verified at all** — the user diff-confirm is the
  only gate before their blind fixes are pushed.
- **`lint` repro needs a local clang-tidy.** The `tidy` target only exists when
  `clang-tidy-22`/`clang-tidy` is on PATH and after a completed configure (it
  references populated FetchContent vars). If `--target tidy` errors with "no
  rule to make target", that is a missing-tool/unconfigured-dir problem, NOT the
  lint failure — install `clang-tidy-22` and re-configure `build-rescue/` first.

## Subagent dispatch

Dispatch one general-purpose subagent per iteration. Model: **Sonnet 4.6** (these
are bounded mechanical fixes). Do NOT use `isolation: "worktree"` — the fix must
land on the user's branch directly.

Prompt template (fill the angle-bracketed slots):

```
CI failure on branch <branch> at <head_sha>, job <job_name>.

Failed log: /tmp/ci-rescue-<run_id>.log (focus on the "<job_name>" section)
Workflow: ci.yml
Triaged category: <category>
Iteration: <n> of <max>

Known priors for this job (if build-macos/build-windows, see the skill's
"Known first-run failures" table): <paste the relevant row(s)>

Prior attempted fixes FOR THIS JOB (do NOT repeat — the signature must change):
- <prior fix summaries for this job only>

Your job:
1. Read the failed log section for this job.
2. Diagnose root cause. Apply [[superpowers:systematic-debugging]] — confirm the
   hypothesis with a code read before editing.
3. Apply a MINIMAL fix. Do NOT broaden scope, refactor unrelated code, or add
   unrequested features.
4. Commit on the current branch: "fix(ci): <one-line summary>".
5. DO NOT PUSH.

Return:
- The commit SHA (git rev-parse HEAD).
- A one-line summary of the change.
- If you cannot fix this mechanically, return the literal token "DIVR" + rationale.

Constraints:
- Respect Orthodox C++ (project CLAUDE.md): any new/edited C++ stays in the C-like
  subset (struct/POD, pointers not refs, C casts, no auto, plain enum, etc.).
- Do NOT weaken the warning posture: never drop /W4, /WX, -Wall/-Wextra/-Wpedantic,
  COMPILE_WARNING_AS_ERROR, or MINESWEEPER_SOLVER_WERROR to make a warning go
  away — fix the code. Fix OUR code (src/*.cc), not the reused sibling sources.
- Do not edit .github/workflows/* unless the log shows a workflow YAML bug.
- Do not amend prior commits. Stay on the current branch; do not switch or rebase.
```

## Stop conditions and exit codes

| Code | Meaning | Action |
|---|---|---|
| 0 | Green — CI passes | `status: idle` |
| 10 | Iteration cap reached | escalate `iteration-cap` |
| 11 | Same per-job signature as prior attempt | escalate `same-error-twice` |
| 12 | Unactionable, preflight failure, or workflow-level failure (no failed jobs) | escalate `unactionable` / `gh-auth-failure` / `workflow-level-failure` |
| 13 | Fix subagent returned `DIVR` or user rejected the diff | escalate `divr` |

On any escalation, AskUserQuestion to surface the scratch-file contents (path +
last 2 attempted fixes + escalation reason) so the user can extend iterations,
switch strategy, or stop.

The user diff-confirm (when `auto_push=false`, and **always** for mac/win) is the
real safety gate: the inline check (HEAD changed + non-empty diff) is the only
mechanical guard against an irrelevant or over-broad subagent edit, since mac/win
fixes have no local test.

## Cross-references

- [[superpowers:systematic-debugging]] — the fix subagent must use it.
- `.github/workflows/ci.yml` — the workflow this skill rescues (jobs `build-linux`,
  `build-macos`, `build-windows`).
- `src/CMakeLists.txt` — per-compiler warning flags (`SOLVER_WARNINGS` = `/W4` on
  MSVC else `-Wall -Wextra -Wpedantic`; `REUSE_WARNINGS` for the reused sibling
  libs), `COMPILE_WARNING_AS_ERROR` gated on `MINESWEEPER_SOLVER_WERROR`, and the
  MSVC `_CRT_SECURE_NO_WARNINGS` define on our targets.
- `cmake/Dependencies.cmake` — FetchContent (minesweeper upstream pinned commit,
  SDL3 [`SDL_X11_XTEST OFF`], SDL_mixer, ImGui, mINI, gtest
  [`gtest_force_shared_crt ON`]); home of the Windows-CRT and Linux-XTEST fixes.
- `CMakeLists.txt` — the `MINESWEEPER_SOLVER_WERROR` and `MINESWEEPER_DIR` options.
- `cmake/Orthodoxy.cmake`, `cmake/StaticAnalysis.cmake` — orthodoxy (local-only,
  not CI) and the `clang-tidy` target (CI `lint` failures).
- `Makefile` — build/test/repro commands.
- `.claude/commands/ci-rescue.md` — slash-command entry.
