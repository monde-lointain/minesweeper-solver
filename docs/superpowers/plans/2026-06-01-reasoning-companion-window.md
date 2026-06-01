# Reasoning Companion Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a second OS window that exposes the solver's train of thought (verdict, the pick and why, what it computed), plus on-board proven-safe/mine markers and hover-to-inspect — without changing the game window's pixel-faithful 1×–4× rendering.

**Architecture:** A second `SDL_Window` + `SDL_Renderer` + ImGui context (ImGui's multiple-contexts pattern; the SDL_Renderer backend has no multi-viewport, verified on both master and docking). One new engine field (`Analysis.exact`) surfaces the existing exact/approx signal. A pure, unit-tested `reasoning_build` extracts a POD `ReasoningView` from the cached `Analysis`; a thin ImGui `reasoning_panel_draw` renders it in the second window. On-board markers extend the existing draw-list `overlay_draw`.

**Tech Stack:** C++17 (Orthodox C++ subset), SDL3, Dear ImGui v1.92.8 (master) + `imgui_impl_sdl3`/`imgui_impl_sdlrenderer3`, GoogleTest, CMake. Spec: `docs/superpowers/specs/2026-06-01-reasoning-companion-window-design.md`.

---

## File Structure

**New files**
- `include/solver/reasoning.h` — POD `struct ReasoningView` + `reasoning_build` decl. SDL/ImGui-free.
- `src/overlay/reasoning.cc` — pure `reasoning_build` (joins `solver_lib`). Orthodox.
- `include/solver/reasoning_panel.h` — `reasoning_panel_draw` decl.
- `src/overlay/reasoning_panel.cc` — thin ImGui renderer (joins the executable). ImGui under the system-include exemption, like `overlay.cc`.
- `tests/engine_exact_test.cc` — `Analysis.exact` true/false via `solver_analyze`.
- `tests/reasoning_test.cc` — `reasoning_build` field extraction.

**Modified files**
- `include/solver/engine.h` — add `bool exact;` to `struct Analysis`.
- `src/engine/engine.cc` — set `out->exact` in `solver_analyze`.
- `src/overlay/overlay.cc` — paint proven-safe/mine dots in `overlay_draw`.
- `include/solver/app.h` — `AppState`: panel window/renderer/contexts, `panel_on`, `hover_x/y`.
- `src/app/app.cc` — second window/context lifecycle, second render pass, event routing, F9, hover, pause-on-minimize-only.
- `src/CMakeLists.txt` — add `reasoning.cc` to `solver_lib`; `reasoning_panel.cc` to the executable.
- `tests/CMakeLists.txt` — register the two new test targets.

---

## Task 0: Branch and commit the spec

**Files:** none (git only)

- [ ] **Step 1: Create the feature branch**

We are on `main`; branch first.

Run:
```bash
git checkout -b feat/reasoning-companion
```

- [ ] **Step 2: Commit the already-written spec**

Run:
```bash
git add docs/superpowers/specs/2026-06-01-reasoning-companion-window-design.md docs/superpowers/plans/2026-06-01-reasoning-companion-window.md
git commit -m "docs: reasoning companion window spec + plan"
```

---

## Task 1: Engine `Analysis.exact` field

**Files:**
- Modify: `include/solver/engine.h` (struct Analysis)
- Modify: `src/engine/engine.cc` (`solver_analyze`)
- Test: `tests/engine_exact_test.cc` (create)
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/engine_exact_test.cc`:
```cpp
/* engine_exact_test.cc — Analysis.exact: true when whole-board probabilities
 * are proven (no component fell back), false when any component is approximate. */
#include <gtest/gtest.h>
#include <string.h>

#include "minesweeper/game.h"
#include "solver/engine.h"

namespace {

/* An ambiguous covered strip: row 1 revealed; rows 0/2 covered with mines at
 * (even,0) and (odd,2). Every middle constraint stays loose (rem 2..3 over 4..6
 * unknowns) so single-point deduction resolves nothing — the whole covered set
 * is ONE undetermined component of 2*W vars. W=10 -> 20 vars (exact path);
 * W=40 -> 80 vars > MAX_COMP_VARS(64) -> fallback_component -> exact == false. */
void mk_ambiguous_strip(struct Board* b, int W) {
  memset(b, 0, sizeof *b);
  b->width = W;
  b->height = 3;
  b->status = GAME_PLAYING;
  int mines = 0;
  for (int x = 0; x < W; ++x) {
    if (x % 2 == 0) {
      b->cells[game_index(b, x, 0)].mine = true;
      ++mines;
    }
    if (x % 2 == 1) {
      b->cells[game_index(b, x, 2)].mine = true;
      ++mines;
    }
  }
  b->mines = mines;
  int rev = 0;
  for (int x = 0; x < W; ++x) {
    int adj = 0;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        int nx = x + dx;
        int ny = 1 + dy;
        if (nx < 0 || ny < 0 || nx >= W || ny >= 3) continue;
        if (b->cells[game_index(b, nx, ny)].mine) ++adj;
      }
    }
    struct Cell* c = &b->cells[game_index(b, x, 1)];
    c->revealed = true;
    c->adjacent = (uint8_t)adj;
    ++rev;
  }
  b->revealed_count = rev;
}

}  // namespace

TEST(AnalysisExact, TrueOnSmallExactFrontier) {
  struct Board b;
  mk_ambiguous_strip(&b, 10);  // 20 vars -> exact enumeration
  struct SolverScratch* s = solver_scratch_create();
  ASSERT_NE(s, nullptr);
  struct Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_TRUE(a.exact);
  solver_scratch_destroy(s);
}

TEST(AnalysisExact, FalseWhenComponentFallsBack) {
  struct Board b;
  mk_ambiguous_strip(&b, 40);  // 80 vars > MAX_COMP_VARS -> fallback
  struct SolverScratch* s = solver_scratch_create();
  ASSERT_NE(s, nullptr);
  struct Analysis a;
  solver_analyze(&b, &a, s);
  EXPECT_FALSE(a.exact);
  solver_scratch_destroy(s);
}

TEST(AnalysisExact, TrueOnTerminalAndStart) {
  struct SolverScratch* s = solver_scratch_create();
  ASSERT_NE(s, nullptr);
  struct Analysis a;
  struct Board b;
  memset(&b, 0, sizeof b);
  b.width = 9;
  b.height = 9;
  b.mines = 10;
  b.status = GAME_PLAYING;
  b.revealed_count = 0;  // EVAL_START
  solver_analyze(&b, &a, s);
  EXPECT_TRUE(a.exact);
  b.status = GAME_WON;  // terminal
  solver_analyze(&b, &a, s);
  EXPECT_TRUE(a.exact);
  solver_scratch_destroy(s);
}
```

Register it — add to `tests/CMakeLists.txt` after the `recommend_test` block (around line 40):
```cmake
add_executable(engine_exact_test engine_exact_test.cc)
target_link_libraries(engine_exact_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(engine_exact_test)
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
make build 2>&1 | tail -20
```
Expected: COMPILE FAILURE — `'struct Analysis' has no member named 'exact'`.

- [ ] **Step 3: Add the field**

In `include/solver/engine.h`, inside `struct Analysis` (after `int eval;`, before the closing `};` around line 45):
```c
  int eval;             /* enum SolverEval */
  bool exact;           /* whole-board probabilities are proven exact (no
                         * component exceeded budget and fell back to the naive
                         * approximation). true for terminal/start/safe states.
                         * Populated by solver_analyze. */
```

- [ ] **Step 4: Set the field in `solver_analyze`**

In `src/engine/engine.cc`, in `solver_analyze` (around line 1218):

After the existing init lines, add the default:
```c
  memset(out, 0, sizeof *out);
  out->best_x = -1;
  out->best_y = -1;
  out->exact = true; /* default: terminal/start/safe have no estimation */
```

At the very end of `solver_analyze`, after `pick_best_move(b, out, &ctx);`:
```c
  pick_best_move(b, out, &ctx);

  /* exact iff the DP ran exactly AND no component used the naive fallback. */
  bool any_fb = false;
  for (int c = 0; c < ctx.ncomp; ++c) {
    if (s->res.fallback[c]) {
      any_fb = true;
      break;
    }
  }
  out->exact = ctx.exact_ok && !any_fb;
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```bash
make build 2>&1 | tail -5 && ctest --test-dir build -R AnalysisExact --output-on-failure
```
Expected: PASS (3 tests).

- [ ] **Step 6: Confirm no regression in the engine suite**

Run:
```bash
ctest --test-dir build -R 'engine|golden|recommend|reduction|infogain' --output-on-failure 2>&1 | tail -15
```
Expected: all PASS (adding a memset-zeroed field touches no marginal).

- [ ] **Step 7: Commit**

```bash
git add include/solver/engine.h src/engine/engine.cc tests/engine_exact_test.cc tests/CMakeLists.txt
git commit -m "feat(engine): Analysis.exact — surface exact-vs-fallback probability signal"
```

---

## Task 2: Pure `reasoning_build` + `ReasoningView`

**Files:**
- Create: `include/solver/reasoning.h`
- Create: `src/overlay/reasoning.cc`
- Modify: `src/CMakeLists.txt` (add to `solver_lib`)
- Test: `tests/reasoning_test.cc` (create)
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the header**

Create `include/solver/reasoning.h`:
```c
/* reasoning.h — pure extraction of a display-ready "train of thought" view from
 * a cached Analysis. NO SDL/ImGui, so it is unit-tested without a window.
 * Orthodox C++: POD, pointers, C headers. */
#ifndef SOLVER_REASONING_H
#define SOLVER_REASONING_H

#include <stdbool.h>

#include "minesweeper/game.h" /* struct Board, game_index */
#include "solver/engine.h"    /* struct Analysis, enum SolverEval */

/* Display-ready snapshot. All percentages are 0..100 ints. */
struct ReasoningView {
  int verdict; /* enum SolverEval */
  bool exact;  /* Analysis.exact */

  bool has_move; /* a covered cell is recommended */
  int move_x;
  int move_y;
  int risk_pct;      /* P(mine) % of the recommended cell */
  int safest_pct;    /* min P(mine) % over covered, non-proven-mine cells */
  bool took_riskier; /* pick risk > safest (band tie-break overrode min-risk) */
  int pick_gain;     /* info_gain of the recommended cell */

  int proven_safe;    /* # covered forced_safe cells */
  int proven_mine;    /* # covered forced_mine cells */
  int frontier;       /* # covered is_frontier cells */
  int interior_pct;   /* interior_prob % */
  int interior_count; /* interior cell count */
  int mines_total;    /* board total mines */

  /* hover inspect (a covered cell under the game-window cursor) */
  bool hover_valid;
  int hover_x;
  int hover_y;
  int hover_pct;
  bool hover_forced_safe;
  bool hover_forced_mine;
  bool hover_frontier;
  int hover_gain;
};

/* Fill `out` from the board + cached analysis. `hover_x/hover_y` are the covered
 * cell under the game cursor, or -1 if none. The recommended move equals
 * solver_recommend_move (same cell the overlay box highlights). Pure: reads
 * b and a, writes only out. */
void reasoning_build(const struct Board* b, const struct Analysis* a,
                     int hover_x, int hover_y, struct ReasoningView* out);

#endif /* SOLVER_REASONING_H */
```

- [ ] **Step 2: Write the failing test**

Create `tests/reasoning_test.cc`:
```cpp
/* reasoning_test.cc — reasoning_build: the pure Analysis -> ReasoningView
 * extraction that feeds the companion panel. */
#include "solver/reasoning.h"

#include <gtest/gtest.h>
#include <string.h>

#include "minesweeper/game.h"
#include "solver/engine.h"

namespace {

void mkboard(struct Board* b, int w, int h) {
  memset(b, 0, sizeof *b);
  b->width = w;
  b->height = h;
  b->mines = 10;
  b->status = GAME_PLAYING;
}

void mkanalysis(struct Analysis* a, const struct Board* b, double p) {
  memset(a, 0, sizeof *a);
  for (int i = 0; i < b->width * b->height; ++i) {
    a->cells[i].mine_prob = p;
  }
  a->eval = EVAL_GUESS;
  a->exact = true;
  a->best_x = 0;
  a->best_y = 0;
  a->best_prob = p;
}

void setp(struct Analysis* a, const struct Board* b, int x, int y, double p) {
  a->cells[game_index(b, x, y)].mine_prob = p;
}
void setgain(struct Analysis* a, const struct Board* b, int x, int y, int g) {
  a->cells[game_index(b, x, y)].info_gain = g;
}

}  // namespace

TEST(Reasoning, VerdictAndExactPassThrough) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  a.eval = EVAL_GUESS;
  a.exact = false;
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  EXPECT_EQ(v.verdict, EVAL_GUESS);
  EXPECT_FALSE(v.exact);
  EXPECT_EQ(v.mines_total, 10);
}

TEST(Reasoning, CountsProvenAndFrontier) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  a.cells[game_index(&b, 1, 1)].forced_safe = true;
  a.cells[game_index(&b, 2, 1)].forced_mine = true;
  a.cells[game_index(&b, 3, 1)].is_frontier = true;
  a.cells[game_index(&b, 1, 1)].is_frontier = true;
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  EXPECT_EQ(v.proven_safe, 1);
  EXPECT_EQ(v.proven_mine, 1);
  EXPECT_EQ(v.frontier, 2);
}

TEST(Reasoning, TookRiskierWhenBandPicksHigherGain) {
  /* Safest cell at 10% zero-gain; a 11% cell (within 2% band) with gain 4 wins.
   * reasoning mirrors solver_recommend_move, so the pick is the 11% cell. */
  struct Board b;
  mkboard(&b, 7, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);
  setp(&a, &b, 1, 1, 0.10);
  setgain(&a, &b, 1, 1, 0);
  setp(&a, &b, 5, 1, 0.11);
  setgain(&a, &b, 5, 1, 4);
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  ASSERT_TRUE(v.has_move);
  EXPECT_EQ(v.move_x, 5);
  EXPECT_EQ(v.move_y, 1);
  EXPECT_EQ(v.risk_pct, 11);
  EXPECT_EQ(v.safest_pct, 10);
  EXPECT_TRUE(v.took_riskier);
  EXPECT_EQ(v.pick_gain, 4);
}

TEST(Reasoning, NotRiskierWhenPickIsSafest) {
  struct Board b;
  mkboard(&b, 7, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.5);
  setp(&a, &b, 2, 1, 0.10);  // unique minimum, zero gain
  struct ReasoningView v;
  reasoning_build(&b, &a, -1, -1, &v);
  ASSERT_TRUE(v.has_move);
  EXPECT_EQ(v.move_x, 2);
  EXPECT_FALSE(v.took_riskier);
  EXPECT_EQ(v.risk_pct, 10);
  EXPECT_EQ(v.safest_pct, 10);
}

TEST(Reasoning, HoverCoveredCellFilled) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  setp(&a, &b, 4, 2, 0.25);
  a.cells[game_index(&b, 4, 2)].is_frontier = true;
  a.cells[game_index(&b, 4, 2)].info_gain = 2;
  struct ReasoningView v;
  reasoning_build(&b, &a, 4, 2, &v);
  ASSERT_TRUE(v.hover_valid);
  EXPECT_EQ(v.hover_x, 4);
  EXPECT_EQ(v.hover_pct, 25);
  EXPECT_TRUE(v.hover_frontier);
  EXPECT_EQ(v.hover_gain, 2);
}

TEST(Reasoning, HoverRevealedOrOutOfRangeInvalid) {
  struct Board b;
  mkboard(&b, 5, 3);
  struct Analysis a;
  mkanalysis(&a, &b, 0.3);
  b.cells[game_index(&b, 1, 1)].revealed = true;
  struct ReasoningView v;
  reasoning_build(&b, &a, 1, 1, &v);  // revealed
  EXPECT_FALSE(v.hover_valid);
  reasoning_build(&b, &a, -1, -1, &v);  // none
  EXPECT_FALSE(v.hover_valid);
  reasoning_build(&b, &a, 99, 99, &v);  // out of range
  EXPECT_FALSE(v.hover_valid);
}
```

Register it — in `tests/CMakeLists.txt` after `engine_exact_test`:
```cmake
add_executable(reasoning_test reasoning_test.cc)
target_link_libraries(reasoning_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(reasoning_test)
```

- [ ] **Step 3: Run test to verify it fails**

Run:
```bash
make build 2>&1 | tail -20
```
Expected: LINK FAILURE — `undefined reference to reasoning_build`.

- [ ] **Step 4: Implement `reasoning_build`**

Create `src/overlay/reasoning.cc`:
```c
/* reasoning.cc — pure Analysis -> ReasoningView extraction (Stream "reasoning").
 * No SDL/ImGui. Orthodox C++: POD, pointers, C headers, index loops. */
#include "solver/reasoning.h"

#include <math.h>
#include <string.h>

#include "solver/recommend.h"
#include "solver/util.h"

static int reasoning_pct(double p) {
  return solver_clampi((int)lround(p * 100.0), 0, 100);
}

void reasoning_build(const struct Board* b, const struct Analysis* a,
                     int hover_x, int hover_y, struct ReasoningView* out) {
  memset(out, 0, sizeof *out);
  out->verdict = a->eval;
  out->exact = a->exact;
  out->interior_count = a->interior_count;
  out->interior_pct = reasoning_pct(a->interior_prob);
  out->mines_total = b->mines;

  int n = b->width * b->height;
  for (int i = 0; i < n; ++i) {
    if (b->cells[i].revealed) {
      continue;
    }
    if (a->cells[i].forced_safe) {
      ++out->proven_safe;
    }
    if (a->cells[i].forced_mine) {
      ++out->proven_mine;
    }
    if (a->cells[i].is_frontier) {
      ++out->frontier;
    }
  }

  int rx = -1;
  int ry = -1;
  if (solver_recommend_move(b, a, &rx, &ry) == 0) {
    int idx = game_index(b, rx, ry);
    out->has_move = true;
    out->move_x = rx;
    out->move_y = ry;
    out->risk_pct = reasoning_pct(a->cells[idx].mine_prob);
    out->pick_gain = a->cells[idx].info_gain;

    double pmin = 2.0;
    for (int i = 0; i < n; ++i) {
      if (b->cells[i].revealed || a->cells[i].forced_mine) {
        continue;
      }
      double p = a->cells[i].mine_prob;
      if (p < pmin) {
        pmin = p;
      }
    }
    out->safest_pct = (pmin <= 1.0) ? reasoning_pct(pmin) : 0;
    out->took_riskier = (a->cells[idx].mine_prob > pmin + 1e-9);
  }

  if (hover_x >= 0 && hover_y >= 0 && hover_x < b->width &&
      hover_y < b->height) {
    int idx = game_index(b, hover_x, hover_y);
    if (!b->cells[idx].revealed) {
      out->hover_valid = true;
      out->hover_x = hover_x;
      out->hover_y = hover_y;
      out->hover_pct = reasoning_pct(a->cells[idx].mine_prob);
      out->hover_forced_safe = a->cells[idx].forced_safe;
      out->hover_forced_mine = a->cells[idx].forced_mine;
      out->hover_frontier = a->cells[idx].is_frontier;
      out->hover_gain = a->cells[idx].info_gain;
    }
  }
}
```

Add to `solver_lib` — in `src/CMakeLists.txt`, the `add_library(solver_lib STATIC ...)` block (around line 25):
```cmake
add_library(solver_lib STATIC
  engine/engine.cc
  engine/recommend.cc
  overlay/overlay_geom.cc
  overlay/reasoning.cc
)
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```bash
make build 2>&1 | tail -5 && ctest --test-dir build -R Reasoning --output-on-failure
```
Expected: PASS (6 tests).

- [ ] **Step 6: Commit**

```bash
git add include/solver/reasoning.h src/overlay/reasoning.cc src/CMakeLists.txt tests/reasoning_test.cc tests/CMakeLists.txt
git commit -m "feat(reasoning): pure ReasoningView extraction (verdict/pick/why/counts/hover)"
```

---

## Task 3: On-board proven-safe/mine markers

**Files:**
- Modify: `src/overlay/overlay.cc` (`overlay_draw`)

No unit test (ImGui draw-list output); verified by build + manual. Keep the change additive — the existing box is untouched.

- [ ] **Step 1: Add the marker loop**

In `src/overlay/overlay.cc`, at the end of `overlay_draw`, after the existing
`dl->AddRect(...)` call (around line 43), add:
```c
  /* Proven-cell markers: certainties (not a risk gradient), so they explain the
   * pick rather than competing with it. Geometry only -> scales 1x-4x. Green =
   * forced_safe, red = forced_mine. */
  int n = b->width * b->height;
  float radius = (float)cell / 6.0f;
  if (radius < 2.0f) {
    radius = 2.0f;
  }
  for (int i = 0; i < n; ++i) {
    if (b->cells[i].revealed) {
      continue;
    }
    bool safe = a->cells[i].forced_safe;
    bool mine = a->cells[i].forced_mine;
    if (!safe && !mine) {
      continue;
    }
    int cx = i % b->width;
    int cy = i / b->width;
    struct OverlayRect cr =
        overlay_cell_rect(lay->grid_x, lay->grid_y, cell, cx, cy);
    ImVec2 center((float)cr.x + (float)cr.w * 0.5f,
                  (float)cr.y + (float)cr.h * 0.5f);
    ImU32 fill = safe ? IM_COL32(46, 158, 46, 235) : IM_COL32(204, 43, 43, 235);
    dl->AddCircleFilled(center, radius, fill, 16);
    dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 180), 16,
                  (float)lay->scale);
  }
```

(`cell`, `dl`, `a`, `b`, `lay` are already in scope from the existing function body. `overlay_cell_rect` is already included via `solver/overlay_geom.h`.)

- [ ] **Step 2: Build**

Run:
```bash
make build 2>&1 | tail -5
```
Expected: clean build (exit 0), Orthodoxy passes.

- [ ] **Step 3: Manual verify**

Run:
```bash
make run
```
Reveal a few cells until the solver proves some safe/mine cells. Expected: green dots on proven-safe covered cells, red dots on proven-mine cells, blue box on the recommended cell. Press F10 — all on-board overlay (box + dots) toggles off/on together. Close the window.

- [ ] **Step 4: Commit**

```bash
git add src/overlay/overlay.cc
git commit -m "feat(overlay): proven-safe/mine dots on the board (geometry, scale-clean)"
```

---

## Task 4: `reasoning_panel_draw` (ImGui renderer)

**Files:**
- Create: `include/solver/reasoning_panel.h`
- Create: `src/overlay/reasoning_panel.cc`
- Modify: `src/CMakeLists.txt` (add to executable)

ImGui output; verified by build + manual (wired into the window in Task 5).

- [ ] **Step 1: Write the header**

Create `include/solver/reasoning_panel.h`:
```c
/* reasoning_panel.h — ImGui renderer for the companion window's reasoning view.
 * The ImGui boundary lives in the .cc (system include). */
#ifndef SOLVER_REASONING_PANEL_H
#define SOLVER_REASONING_PANEL_H

#include "solver/reasoning.h"

/* Render the reasoning view as ImGui widgets into the current window. Call
 * between ImGui::Begin/End for the companion window. */
void reasoning_panel_draw(const struct ReasoningView* v);

#endif /* SOLVER_REASONING_PANEL_H */
```

- [ ] **Step 2: Implement the renderer**

Create `src/overlay/reasoning_panel.cc`:
```c
/* reasoning_panel.cc — ImGui rendering of ReasoningView. ImGui is a SYSTEM
 * include so Orthodoxy ignores its declarations; we only call its API. */
#include "solver/reasoning_panel.h"

#include <imgui.h>

#include "solver/engine.h" /* enum SolverEval */

static const char* verdict_text(int v) {
  switch (v) {
    case EVAL_START:
      return "START";
    case EVAL_SAFE:
      return "SAFE";
    case EVAL_GUESS:
      return "GUESS";
    case EVAL_SOLVED:
      return "SOLVED";
    case EVAL_LOST:
      return "LOST";
    default:
      return "?";
  }
}

static ImVec4 verdict_color(int v) {
  switch (v) {
    case EVAL_SAFE:
      return ImVec4(0.51f, 0.78f, 0.52f, 1.0f); /* green */
    case EVAL_GUESS:
      return ImVec4(1.0f, 0.72f, 0.30f, 1.0f); /* amber */
    case EVAL_LOST:
      return ImVec4(0.90f, 0.40f, 0.40f, 1.0f); /* red */
    default:
      return ImVec4(0.80f, 0.84f, 0.90f, 1.0f); /* slate */
  }
}

void reasoning_panel_draw(const struct ReasoningView* v) {
  /* Verdict */
  ImGui::TextColored(verdict_color(v->verdict), "%s", verdict_text(v->verdict));
  ImGui::SameLine();
  ImGui::TextDisabled(v->exact ? "[exact]" : "[approx]");

  if (v->verdict == EVAL_GUESS) {
    ImGui::TextDisabled("no proven-safe cell - must gamble");
  } else if (v->verdict == EVAL_SAFE) {
    ImGui::TextDisabled("a proven move exists - no guess");
  }

  ImGui::Separator();

  /* Recommendation */
  if (v->has_move) {
    ImGui::Text("Move (%d, %d)", v->move_x, v->move_y);
    ImGui::Text("risk %d%% mine", v->risk_pct);
    ImGui::ProgressBar((float)v->risk_pct / 100.0f, ImVec2(-1.0f, 6.0f), "");
    if (v->verdict == EVAL_SAFE) {
      ImGui::BulletText("forced safe by deduction");
    } else if (v->verdict == EVAL_GUESS) {
      if (v->took_riskier) {
        ImGui::BulletText("took +%d%% over safest (%d%%)",
                          v->risk_pct - v->safest_pct, v->safest_pct);
      } else {
        ImGui::BulletText("lowest-risk cell");
      }
      if (v->pick_gain > 0) {
        ImGui::BulletText("unlocks %d more cell%s if safe", v->pick_gain,
                          v->pick_gain == 1 ? "" : "s");
      }
    }
  } else {
    ImGui::TextDisabled("(no move)");
  }

  ImGui::Separator();

  /* This turn */
  ImGui::Text("proven safe   %d", v->proven_safe);
  ImGui::Text("proven mine   %d", v->proven_mine);
  ImGui::Text("frontier      %d", v->frontier);
  if (v->interior_count > 0) {
    ImGui::Text("interior      %d%% x %d", v->interior_pct, v->interior_count);
  }
  ImGui::Text("mines total   %d", v->mines_total);

  ImGui::Separator();

  /* Hover inspect */
  if (v->hover_valid) {
    ImGui::Text("cell (%d, %d): %d%% mine", v->hover_x, v->hover_y,
                v->hover_pct);
    if (v->hover_forced_safe) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.51f, 0.78f, 0.52f, 1.0f), "(safe)");
    } else if (v->hover_forced_mine) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.90f, 0.40f, 0.40f, 1.0f), "(mine)");
    }
    if (v->hover_frontier && v->hover_gain > 0) {
      ImGui::Text("  unlocks %d if safe", v->hover_gain);
    }
  } else {
    ImGui::TextDisabled("hover a covered cell to inspect");
  }
}
```

Add to the executable — in `src/CMakeLists.txt`, the `add_executable(minesweeper_solver ...)` block (around line 38):
```cmake
add_executable(minesweeper_solver
  main.cc
  app/app.cc
  overlay/overlay.cc
  overlay/reasoning_panel.cc
)
```

- [ ] **Step 3: Build**

Run:
```bash
make build 2>&1 | tail -8
```
Expected: clean build. (Not yet visible — Task 5 wires it into the window.)

- [ ] **Step 4: Commit**

```bash
git add include/solver/reasoning_panel.h src/overlay/reasoning_panel.cc src/CMakeLists.txt
git commit -m "feat(overlay): ImGui reasoning panel renderer"
```

---

## Task 5A: Second window + renderer + ImGui context (lifecycle)

**Files:**
- Modify: `include/solver/app.h` (`AppState`)
- Modify: `src/app/app.cc` (`app_init`, `app_quit`)

- [ ] **Step 1: Add AppState fields**

In `include/solver/app.h`, inside `struct AppState`, after the `--- solver additions ---` block (after `bool overlay_on;`, around line 71):
```c
  bool overlay_on; /* F10 toggles the analysis overlay */

  /* --- companion reasoning window --- */
  SDL_Window* panel_window;
  SDL_Renderer* panel_renderer;
  void* ctx_game;  /* ImGuiContext* (opaque here to avoid an ImGui include) */
  void* ctx_panel; /* ImGuiContext* */
  bool panel_on;   /* F9 toggles the companion window */
  int hover_x;     /* covered cell under the game cursor, -1 if none */
  int hover_y;
```

- [ ] **Step 2: Capture the game context + create the companion in `app_init`**

In `src/app/app.cc`, in `app_init`, find the existing ImGui setup (around line 224-231):
```c
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO* io = &ImGui::GetIO();
  io->IniFilename = NULL; /* no imgui.ini */
  ImGui_ImplSDL3_InitForSDLRenderer(s->window, s->renderer);
  ImGui_ImplSDLRenderer3_Init(s->renderer);
  ui_apply_theme();
```
Replace with (capture ctx_game, then build the second window/context):
```c
  IMGUI_CHECKVERSION();
  s->ctx_game = (void*)ImGui::CreateContext();
  ImGuiIO* io = &ImGui::GetIO();
  io->IniFilename = NULL; /* no imgui.ini */
  ImGui_ImplSDL3_InitForSDLRenderer(s->window, s->renderer);
  ImGui_ImplSDLRenderer3_Init(s->renderer);
  ui_apply_theme();

  /* Companion reasoning window: its own window + renderer + ImGui context
   * (the SDL_Renderer backend has no multi-viewport). vsync OFF (static
   * content) so it never double-stalls the game's vsync. */
  s->panel_window =
      SDL_CreateWindow("Solver - Reasoning", 300, 380, 0);
  if (s->panel_window != NULL) {
    s->panel_renderer = SDL_CreateRenderer(s->panel_window, NULL);
  }
  if (s->panel_renderer != NULL) {
    s->ctx_panel = (void*)ImGui::CreateContext();
    ImGui::SetCurrentContext((ImGuiContext*)s->ctx_panel);
    ImGuiIO* pio = &ImGui::GetIO();
    pio->IniFilename = NULL;
    ImGui_ImplSDL3_InitForSDLRenderer(s->panel_window, s->panel_renderer);
    ImGui_ImplSDLRenderer3_Init(s->panel_renderer);
    ui_apply_theme();
    ImGui::SetCurrentContext((ImGuiContext*)s->ctx_game);
    s->panel_on = true;
  }
  s->hover_x = -1;
  s->hover_y = -1;
```

- [ ] **Step 3: Position the companion right of the game window**

In `app_init`, after the existing `SDL_ShowWindow(s->window);` (around line 241), add:
```c
  if (s->panel_window != NULL) {
    int gx = 0;
    int gy = 0;
    int gw = 0;
    int gh = 0;
    SDL_GetWindowPosition(s->window, &gx, &gy);
    SDL_GetWindowSize(s->window, &gw, &gh);
    SDL_SetWindowPosition(s->panel_window, gx + gw + 8, gy);
    SDL_ShowWindow(s->panel_window);
  }
```

- [ ] **Step 4: Teardown in `app_quit`**

In `src/app/app.cc`, in `app_quit`, replace the existing ImGui shutdown block:
```c
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
```
with (tear down the panel context first, then the game context):
```c
  if (s->ctx_panel != NULL) {
    ImGui::SetCurrentContext((ImGuiContext*)s->ctx_panel);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext((ImGuiContext*)s->ctx_panel);
    s->ctx_panel = NULL;
  }
  ImGui::SetCurrentContext((ImGuiContext*)s->ctx_game);
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext((ImGuiContext*)s->ctx_game);
  s->ctx_game = NULL;
```

Also, before `SDL_DestroyRenderer(s->renderer);` (around line 581), destroy the panel window/renderer:
```c
  if (s->panel_renderer != NULL) {
    SDL_DestroyRenderer(s->panel_renderer);
  }
  if (s->panel_window != NULL) {
    SDL_DestroyWindow(s->panel_window);
  }
```

- [ ] **Step 5: Build**

Run:
```bash
make build 2>&1 | tail -8
```
Expected: clean build.

- [ ] **Step 6: Manual verify**

Run:
```bash
make run
```
Expected: two windows open — the game and an empty dark "Solver - Reasoning" window to its right. Closing the GAME window quits. (The companion is still empty and its ✕ may quit — fixed in 5C.) Quit via the game window.

- [ ] **Step 7: Commit**

```bash
git add include/solver/app.h src/app/app.cc
git commit -m "feat(app): second SDL window + ImGui context for the reasoning companion"
```

---

## Task 5B: Render the reasoning panel each frame

**Files:**
- Modify: `src/app/app.cc` (`app_iterate`, includes)

- [ ] **Step 1: Include the panel headers**

In `src/app/app.cc` includes (near line 28), add:
```c
#include "solver/overlay.h"
#include "solver/reasoning.h"
#include "solver/reasoning_panel.h"
```
(`solver/overlay.h` is already present; add the two reasoning headers.)

- [ ] **Step 2: Render the second context after the game present**

In `app_iterate`, after the existing game-window present (`SDL_RenderPresent(s->renderer);`, around line 559) and before `return ...`, add:
```c
  /* Companion reasoning window: separate context, separate renderer. */
  if (s->panel_on && s->panel_window != NULL && s->ctx_panel != NULL) {
    struct ReasoningView rv;
    reasoning_build(&s->board, &s->analysis, s->hover_x, s->hover_y, &rv);

    ImGui::SetCurrentContext((ImGuiContext*)s->ctx_panel);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGuiIO* pio = &ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(pio->DisplaySize);
    ImGui::Begin("reasoning", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    reasoning_panel_draw(&rv);
    ImGui::End();

    SDL_SetRenderDrawColor(s->panel_renderer, 30, 30, 30, 255);
    SDL_RenderClear(s->panel_renderer);
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),
                                          s->panel_renderer);
    SDL_RenderPresent(s->panel_renderer);

    ImGui::SetCurrentContext((ImGuiContext*)s->ctx_game);
  }
```

- [ ] **Step 3: Build**

Run:
```bash
make build 2>&1 | tail -8
```
Expected: clean build.

- [ ] **Step 4: Manual verify**

Run:
```bash
make run
```
Expected: the companion now shows the verdict, recommendation + why, and "this turn" counts, updating live as you reveal cells. (Hover section says "hover a covered cell to inspect" — wired in 5D.)

- [ ] **Step 5: Commit**

```bash
git add src/app/app.cc
git commit -m "feat(app): render the reasoning companion each frame"
```

---

## Task 5C: Event routing, F9 toggle, close = hide

**Files:**
- Modify: `src/app/app.cc` (`app_event`)

- [ ] **Step 1: Add a window-id helper**

In `src/app/app.cc`, above `app_event` (around line 378), add:
```c
/* The window an event targets, or 0 if it has none. Used to route ImGui event
 * processing to the matching context. */
static SDL_WindowID app_event_window(const SDL_Event* e) {
  switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
      return e->motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return e->button.windowID;
    case SDL_EVENT_MOUSE_WHEEL:
      return e->wheel.windowID;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      return e->key.windowID;
    case SDL_EVENT_TEXT_INPUT:
      return e->text.windowID;
    default:
      if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) {
        return e->window.windowID;
      }
      return 0;
  }
}
```

- [ ] **Step 2: Route ImGui processing + handle companion events at the top of `app_event`**

In `src/app/app.cc`, replace the start of `app_event`:
```c
SDL_AppResult app_event(struct AppState* s, SDL_Event* event) {
  ImGui_ImplSDL3_ProcessEvent(event);
  ImGuiIO* io = &ImGui::GetIO();
```
with:
```c
SDL_AppResult app_event(struct AppState* s, SDL_Event* event) {
  SDL_WindowID wid = app_event_window(event);
  bool is_panel = (s->panel_window != NULL &&
                   wid == SDL_GetWindowID(s->panel_window));

  /* Feed the event to the context that owns the target window. */
  ImGui::SetCurrentContext(
      (ImGuiContext*)(is_panel ? s->ctx_panel : s->ctx_game));
  ImGui_ImplSDL3_ProcessEvent(event);
  ImGui::SetCurrentContext((ImGuiContext*)s->ctx_game);

  /* Companion window: ✕ hides it (never quits the app); otherwise ImGui owns
   * its interactions. */
  if (is_panel) {
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      SDL_HideWindow(s->panel_window);
      s->panel_on = false;
    }
    return SDL_APP_CONTINUE;
  }

  /* Game window ✕ quits. */
  if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
    return SDL_APP_SUCCESS;
  }

  ImGuiIO* io = &ImGui::GetIO();
```
(The rest of `app_event` — the existing `switch (event->type)` — is unchanged and now only runs for game-window/global events.)

- [ ] **Step 3: Make F9 toggle the companion (works regardless of focus)**

In `app_event`, in the `SDL_EVENT_KEY_DOWN` handling, find the F10 branch (around line 403):
```c
        } else if (event->key.key == SDLK_F10) {
          s->overlay_on = !s->overlay_on; /* (5) */
        }
```
Replace with:
```c
        } else if (event->key.key == SDLK_F10) {
          s->overlay_on = !s->overlay_on; /* (5) */
        } else if (event->key.key == SDLK_F9) {
          s->panel_on = !s->panel_on;
          if (s->panel_window != NULL) {
            if (s->panel_on) {
              SDL_ShowWindow(s->panel_window);
            } else {
              SDL_HideWindow(s->panel_window);
            }
          }
        }
```

- [ ] **Step 4: Build**

Run:
```bash
make build 2>&1 | tail -8
```
Expected: clean build.

- [ ] **Step 5: Manual verify**

Run:
```bash
make run
```
Expected: clicking/dragging the companion does NOT affect the board. The companion ✕ hides it (game keeps running); F9 re-summons / hides it. F10 still toggles the on-board overlay. Closing the game window quits.

- [ ] **Step 6: Commit**

```bash
git add src/app/app.cc
git commit -m "feat(app): route events per window; F9 toggle; companion close = hide"
```

---

## Task 5D: Hover tracking + pause only on game minimize

**Files:**
- Modify: `src/app/app.cc` (`app_event`)

- [ ] **Step 1: Track the hovered cell on game-window motion**

In `src/app/app.cc`, in `app_event`'s `switch`, the existing `SDL_EVENT_MOUSE_MOTION` case (around line 418) is:
```c
    case SDL_EVENT_MOUSE_MOTION:
      if (!io->WantCaptureMouse && (s->pressing_board || s->chord_active)) {
        app_press_update(s, event->motion.x, event->motion.y);
      }
      break;
```
Replace with (also update hover from the cell under the cursor):
```c
    case SDL_EVENT_MOUSE_MOTION: {
      if (!io->WantCaptureMouse && (s->pressing_board || s->chord_active)) {
        app_press_update(s, event->motion.x, event->motion.y);
      }
      struct Layout lay;
      int cx = 0;
      int cy = 0;
      app_compute_layout(s, &lay);
      if (render_cell_at(&s->board, &lay, event->motion.x, event->motion.y, &cx,
                         &cy)) {
        s->hover_x = cx;
        s->hover_y = cy;
      } else {
        s->hover_x = -1;
        s->hover_y = -1;
      }
      break;
    }
```

- [ ] **Step 2: Clear hover when the cursor leaves the game window**

In the `switch`, add a case (next to the motion case):
```c
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      s->hover_x = -1;
      s->hover_y = -1;
      break;
```

- [ ] **Step 3: Pause only on game-window minimize, not on focus loss**

In `app_event`'s `switch`, replace the existing pause cases:
```c
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      app_set_paused(s, true);
      break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      app_set_paused(s, false);
      break;
```
with (focus loss no longer pauses — clicking the companion must not pause the
game; only an actual minimize of the game window does):
```c
    case SDL_EVENT_WINDOW_MINIMIZED:
      app_set_paused(s, true);
      break;
    case SDL_EVENT_WINDOW_RESTORED:
      app_set_paused(s, false);
      break;
```

- [ ] **Step 4: Build**

Run:
```bash
make build 2>&1 | tail -8
```
Expected: clean build.

- [ ] **Step 5: Manual verify**

Run:
```bash
make run
```
Expected: hovering a covered cell fills the companion's inspect line (P(mine), proven/frontier, unlocks); moving off the board clears it. Clicking the companion no longer pauses the game timer. Minimizing the game window pauses; restoring resumes.

- [ ] **Step 6: Commit**

```bash
git add src/app/app.cc
git commit -m "feat(app): hover-to-inspect feed; pause only on game minimize"
```

---

## Task 6: Final verification

**Files:** none (verification only)

- [ ] **Step 1: Full build with warnings-as-errors + Orthodoxy**

Run:
```bash
make build 2>&1 | tail -15
```
Expected: exit 0, no warnings, Orthodoxy clean (our targets only).

- [ ] **Step 2: Full test suite**

Run:
```bash
ctest --test-dir build --output-on-failure 2>&1 | tail -25
```
Expected: 100% pass, including `AnalysisExact`, `Reasoning`, and the existing engine/recommend/golden/bench suites (no regressions).

- [ ] **Step 3: Format**

Run:
```bash
make format
git diff --stat
```
Expected: no unexpected reformatting of the new files (commit any formatting deltas).

- [ ] **Step 4: Manual matrix**

Run `make run` and verify:
- Two windows; companion shows verdict/recommendation/why/counts, live.
- Green/red proven dots + blue box on the board.
- Hover a covered cell → companion inspect updates; leave board → clears.
- F9 toggles companion; ✕ on companion hides (app runs); F10 toggles on-board overlay; game ✕ quits.
- Change scale to 1× and 4× (menu): box + dots scale crisply; companion text stays readable; companion not pinned to game size.
- Companion focus does not pause the game timer; game minimize does.

- [ ] **Step 5: Commit any format deltas**

```bash
git add -A
git commit -m "style: clang-format the reasoning companion sources" || echo "nothing to format"
```

---

## Self-Review

**Spec coverage:**
- Separate OS window via 2nd context → Tasks 5A/5B. ✓
- `Analysis.exact` (one engine field) → Task 1. ✓
- Pure, testable reasoning extraction → Task 2. ✓
- On-board proven markers (box kept + dots, F10) → Task 3. ✓
- ImGui panel content (verdict/recommendation+why/this-turn/hover) → Tasks 4, 5B. ✓
- Hover-to-inspect → Tasks 4 (render), 5D (feed). ✓
- Companion opens on at launch → Task 5A. ✓
- F9 toggle, ✕ hides not quits, no sibling menu edit → Task 5C. ✓
- Pause only on game minimize (companion focus safe) → Task 5D. ✓
- VSync game-only → Task 5A (panel renderer created without `SDL_SetRenderVSync`). ✓
- Tests: reasoning_build all evals + band tie-break + hover; exact true/false → Tasks 1, 2. ✓
- Orthodoxy boundaries (pure reasoning in solver_lib; ImGui localized) → Tasks 2, 4. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code; commands have expected output. ✓

**Type consistency:** `ReasoningView` fields match across `reasoning.h` (Task 2), `reasoning.cc` (Task 2), `reasoning_panel.cc` (Task 4), and the test (Task 2). `s->ctx_game`/`s->ctx_panel` typed `void*` in `app.h` and consistently cast to `ImGuiContext*` at use. `app_event_window` returns `SDL_WindowID`, compared against `SDL_GetWindowID`. `Analysis.exact` set in engine (Task 1), read in `reasoning_build` (Task 2). ✓

## Unresolved questions

1. Companion default size (300×380) and right-of-game placement: clamp to display bounds if the game window is near a screen edge — tune during Task 5A manual verify.
2. Proven-dot radius (`cell/6`, min 2px) and halo thickness (`scale`): eyeball at 1× and 4× in Task 6; adjust constants if cramped.
3. `exact` is board-level; the hover inspect does not show a per-cell "(approx)" note in v1 (deferred, per spec).
