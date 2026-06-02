#!/bin/bash
# Annotate the latest callgrind output from `make profile` into reports/.
# Produces a per-source annotation (callgrind_annotate --auto=yes) and a short
# top-functions summary, both timestamped so successive runs can be compared.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROFILING_DIR="$(dirname "$SCRIPT_DIR")"
REPORTS_DIR="$PROFILING_DIR/reports"

mkdir -p "$REPORTS_DIR"

# Pick the newest raw output (not the symlink) and reuse its timestamp.
latest="$(ls -t "$PROFILING_DIR"/callgrind.out.minesweeper.* 2>/dev/null | head -1 || true)"
if [ -z "${latest:-}" ]; then
  echo "no callgrind.out.minesweeper.* found — run 'make profile' first" >&2
  exit 1
fi
TS="${PROFILE_TIMESTAMP:-${latest##*.}}"

annotate="$REPORTS_DIR/minesweeper_annotate_${TS}.txt"
summary="$REPORTS_DIR/minesweeper_summary_${TS}.txt"

echo "annotating $latest ..."
callgrind_annotate --auto=yes --threshold=99 "$latest" > "$annotate" 2>&1 || true
echo "  $annotate"

{
  echo "=== callgrind summary (timestamp $TS) ==="
  echo "source: $latest"
  echo
  grep -m1 '^events:' "$latest" || true
  grep -m1 '^summary:' "$latest" || true
  echo
  echo "--- hottest functions (self cost) ---"
  # callgrind_annotate lists functions after the 'Ir  file:function' header.
  awk '/^[ ]*Ir[ ]/{f=1;next} f&&/^-+$/{next} f{print} f&&NF==0{exit}' \
    "$annotate" | head -30
} > "$summary"
echo "  $summary"

echo
echo "open interactively with: kcachegrind $PROFILING_DIR/callgrind.out.minesweeper"
