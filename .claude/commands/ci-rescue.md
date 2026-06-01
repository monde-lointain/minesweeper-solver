---
allowed-tools: Bash(gh:*), Bash(git:*), Bash(jq:*), Bash(grep:*), Bash(sha256sum:*), Bash(sed:*), Bash(sort:*), Bash(cut:*), Bash(printf:*), Bash(echo:*), Bash(mapfile:*), Bash(cmake:*), Bash(ctest:*), Bash(make:*)
description: "Pull failed ci.yml logs and iterate fixes until green or escalation. Usage: /ci-rescue [max_iterations] [--auto-push]"
disable-model-invocation: false
---

Run the CI-rescue loop for the current branch's latest `ci.yml` run.

Args (all optional):
- `max_iterations` (positional integer) — cap on total pushes across all jobs. Default: 6.
- `--auto-push` — push each locally-verified fix without asking. Default: ask.
  Applies to `build-linux` (Linux) only; `build-macos` / `build-windows` are never
  auto-pushed — they cannot be built on this Linux host, so the diff-confirm is
  the only safety gate.

No run-id or PR-number arg: the branch + the `ci.yml` workflow filter + the
`headSha == HEAD` staleness guard resolve the correct run.

Steps:

1. Invoke `Skill(skill: "rescuing-ci-failures")` to load the algorithm, triage
   rules, error-signature dedup, stop conditions, local-verify-first mapping,
   known first-run failures, and the subagent dispatch template.

2. Parse args. Defaults: `max_iterations=6`, `auto_push=false`. Workflow is fixed
   to `ci.yml`.

3. Run the algorithm. State is inline (TodoWrite + `/tmp/ci-rescue-<branch>.json`);
   there is no helper script and no committed state.

4. On exit:
   - **Green** (code 0): report the `attempted_fixes` summary + final `head_sha`;
     set the scratch file `status: idle`.
   - **Escalation** (codes 10–13): AskUserQuestion with the escalation reason, the
     last 2 `attempted_fixes`, and the relevant log path
     (`/tmp/ci-rescue-<run_id>.log`). Leave the scratch file in place for
     postmortem; tell the user it can be deleted to reset.

Notes:
- On `main`: warn (CI red on main is usually an incident) and confirm via
  AskUserQuestion before proceeding.
- Preconditions: clean working tree and local `HEAD` == the failing run's head SHA
  (otherwise the fix would target a baseline the log doesn't describe).
- `build-macos` / `build-windows` fixes are blind (no local repro) — the next CI
  run is their only test; expect more iterations on those legs.

Examples:

```
/ci-rescue
/ci-rescue --auto-push
/ci-rescue 3
/ci-rescue 10 --auto-push
```
