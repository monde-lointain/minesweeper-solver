/* difficulty.h — single source of truth for the fixed Beginner/Intermediate/
 * Expert board presets, shared by the GUI app (app.cc) and the benchmark CLI
 * (bench/args.cc) so the two can never disagree on what a difficulty means.
 *
 * Header-only (static inline), like util.h, so both targets use it without a
 * link dependency. Orthodox C++: free functions, plain enum, pointers, C. */
#ifndef SOLVER_DIFFICULTY_H
#define SOLVER_DIFFICULTY_H

#include <stddef.h> /* NULL */

#include "minesweeper/types.h" /* enum Difficulty */

/* Fill (w,h,mines) for a fixed preset and return true; return false for
 * DIFF_CUSTOM (the caller supplies its own dimensions). `diff` is an enum
 * Difficulty value. */
static inline bool difficulty_preset_dims(int diff, int* w, int* h,
                                          int* mines) {
  if (diff == DIFF_INTERMEDIATE) {
    *w = 16;
    *h = 16;
    *mines = 40;
    return true;
  }
  if (diff == DIFF_EXPERT) {
    *w = 30;
    *h = 16;
    *mines = 99;
    return true;
  }
  if (diff == DIFF_BEGINNER) {
    *w = 9;
    *h = 9;
    *mines = 10;
    return true;
  }
  return false; /* DIFF_CUSTOM or unknown */
}

/* CLI/preset name for a fixed preset, or NULL for DIFF_CUSTOM/unknown. */
static inline const char* difficulty_preset_name(int diff) {
  if (diff == DIFF_BEGINNER) {
    return "beginner";
  }
  if (diff == DIFF_INTERMEDIATE) {
    return "intermediate";
  }
  if (diff == DIFF_EXPERT) {
    return "expert";
  }
  return NULL;
}

#endif /* SOLVER_DIFFICULTY_H */
