# Reasoning Companion Window — Design

Date: 2026-06-01
Status: approved (brainstorm), pending implementation plan

## Goal

Expose the solver's "train of thought" — what it proved, what it recommends and
why, what it computed — without compromising the pixel-faithful game window or
its 1×–4× integer scaling. Minimal on-board footprint; rich reasoning lives in a
separate, independently-sized OS window.

## Why a separate OS window

The game renders zero real text — every glyph (cell numbers, LED digits, smiley)
is a bitmap sprite blitted at integer scale. The only live font is ImGui's
(menu bar), fixed-size regardless of game scale. The game window is also sized
exactly to the board (1× Beginner ≈ 165px wide). So verbose reasoning text cannot
live legibly on/over the board across scales. A separate, resizable window
dissolves the tension: the game stays faithful (box + scaled markers only); the
companion holds unlimited text at a comfortable size.

ImGui multi-viewport (auto-detached windows) is **unavailable** here:
`imgui_impl_sdlrenderer3` lists "Renderer: Multi-viewport support" as missing on
**both** master and the docking branch (verified in
`~/development/repos/imgui/backends/imgui_impl_sdlrenderer3.cpp:17-18`). The
blocker is the SDL_Renderer backend, not the branch — so docking is a non-starter
and we stay on master v1.92.8. Instead we use ImGui's documented **multiple-
contexts** approach: a second `SDL_Window` + `SDL_Renderer` + ImGui context.

## Decisions (locked)

1. **Game window:** keep the blue recommended-move box AND add green/red dots on
   `forced_safe`/`forced_mine` cells (pure geometry, scales 1×–4×). Tied to the
   existing F10 on-board toggle.
2. **Exact/approx chip:** included. Requires one new field on the frozen
   `Analysis` struct (the only engine touch).
3. **Hover-to-inspect:** in v1. Hover a covered cell in the game window → the
   companion shows that cell's P(mine) / proven status / info-gain.
4. **Companion opens on at launch**, positioned to the right of the game window.

## Architecture

Two OS windows, two ImGui contexts, one shared cached `Analysis`.

- `ctx_game` — existing game window: board + chrome + menu + dialogs + on-board
  overlay (background draw-list: box + proven markers).
- `ctx_panel` — new companion window: one full-window ImGui window rendering the
  reasoning panel.

Per `app_iterate`: render `ctx_game` → present game renderer; if `panel_on`,
render `ctx_panel` → present panel renderer.

Event routing: each SDL event is dispatched by `event->window.windowID` (mapped
via `SDL_GetWindowFromID`) to the owning context — `SetCurrentContext(ctx)` then
`ImGui_ImplSDL3_ProcessEvent`. Game-window events keep today's handling; F9 is
handled globally; companion events are consumed by ImGui.

## Components and files

### Engine (one field)
- `include/solver/engine.h`: add `bool exact;` to `struct Analysis`. Semantics:
  the whole-board probabilities are exact, i.e. `exact_ok && no component used
  the naive fallback`. Authorized amendment to the frozen contract; document
  inline.
- `src/engine/engine.cc`: set `out->exact` in the finalize path (near where
  `eval` is set, `pick_best_move`/`solver_analyze`). The signal already exists
  internally (`ctx.exact_ok` at ~851/1239; per-component `fallback[c]` at ~107/
  795; the per-cell distinction is computed in `write_cell_probs` ~1152-1160) —
  this only surfaces a board-level summary. EVAL_START/SAFE/SOLVED/LOST set
  `exact = true` trivially (no probabilistic estimation in play).

### Pure reasoning builder (new, testable — no ImGui)
- `include/solver/reasoning.h`: POD `struct ReasoningView` — verdict enum, move
  x/y, risk percent, fixed-buffer rationale lines (`char[N][M]` + count), turn
  counts (proven_safe, proven_mine, frontier_cells, components, interior_pct,
  interior_count, mines_left), `bool exact`, and hover fields (valid, x/y,
  mine_pct, forced_safe/mine, is_frontier, info_gain).
- `src/overlay/reasoning.cc`: `void reasoning_build(const struct Board*, const
  struct Analysis*, int hover_x, int hover_y, struct ReasoningView* out)`. Pure
  function of inputs; mirrors `overlay_geom`'s testable style. Composes rationale
  strings: SAFE → "forced safe by deduction"; GUESS → lowest-risk, plus the band
  tie-break ("took +X% over safest Y% to unlock N cells") when the pick's prob
  exceeds `pmin`, plus cascade/connectivity; START → "opening pick"; SOLVED/LOST
  → one-line outcome.

### Panel renderer (new, thin ImGui)
- `src/overlay/reasoning_panel.cc`: `void reasoning_panel_draw(const struct
  ReasoningView*)`. Lays out Verdict (+exact chip) / Recommendation + why / This
  turn / Hover inspect. ImGui confined here under the existing system-include
  exemption (HERESY-localized like `overlay.cc`).

### On-board markers
- `src/overlay/overlay.cc`: extend `overlay_draw` — after the box, scan covered
  cells and paint `AddCircleFilled` dots (green `forced_safe`, red
  `forced_mine`), radius ∝ `cell = BLOCK_PX*scale` (e.g. `cell/6`, min ~2px),
  with a thin white halo. Reuses `overlay_cell_rect`. Tied to F10.

### App glue
- `include/solver/app.h` (`struct AppState`): add `SDL_Window* panel_window`,
  `SDL_Renderer* panel_renderer`, `ImGuiContext* ctx_game`, `ImGuiContext*
  ctx_panel`, `bool panel_on`, `int hover_x`, `int hover_y`.
- `src/app/app.cc`: create the second window/renderer/context in `app_init`
  (position right of game window, dark clear color, **vsync off**); render the
  second pass in `app_iterate`; route events by windowID in `app_event`; track
  `hover_x/hover_y` from `render_cell_at` on game-window motion; **F9** toggles
  `panel_on` (+ `SDL_HideWindow`/`SDL_ShowWindow`); teardown in `app_quit`.

## Data flow

Board change → existing `AnalysisKey` signature → `solver_analyze_infogain`
(now also sets `exact`) → one cached `Analysis`. The game overlay (box+markers)
and `reasoning_build` both read it — zero extra engine cost. Hover is a pure read
of `analysis.cells[game_index(hover)]`; never triggers recompute.

## Lifecycle and edge cases

- **Toggle:** companion on at launch. F9 hides/shows (not destroy). No menu entry
  — the menu bar is the shared sibling `ui_menu_bar`; the fork-only constraint
  ([[solver-is-overlay-plugin]]) says don't edit sibling modules. F9 mirrors the
  existing F10 pattern; document it in About/Help text if convenient.
- **Close = hide, not quit:** `SDL_EVENT_WINDOW_CLOSE_REQUESTED` on the companion
  → hide + clear `panel_on`; on the game window → quit. Handle per-window
  explicitly; don't rely on `SDL_EVENT_QUIT` alone.
- **Pause:** companion focus must NOT pause the game timer. Pause only on
  game-window MINIMIZED; drop or gate the focus-lost→pause rule so it fires only
  when neither window is focused.
- **VSync:** game renderer keeps vsync; panel renderer vsync OFF (static content)
  to avoid a double wait-stall.
- **Scaling:** box, markers, and any badge derive from `cell = BLOCK_PX*scale` →
  crisp at 1×–4×. Companion text is ImGui-fixed-size, independent of game scale.

## Testing

- New `tests/reasoning_test.cc` (pure `reasoning_build`): verdict mapping for all
  five evals; SAFE rationale = forced; GUESS rationale distinguishes plain
  lowest-risk vs band tie-break ("+X% to unlock N"); hover field extraction for
  covered/revealed/none; `exact` true and false.
- Engine: assert `out->exact == (exact_ok && !any-fallback)` on a board that
  forces a fallback component, and `true` on a small exact board. Existing
  differential/forced-safe tests unchanged (no marginal touched).
- Manual: build; two windows; F9 toggles companion, F10 toggles on-board layer;
  ✕ hides companion (app keeps running); hover updates the inspect section;
  geometry correct at 1× and 4×; game timer not paused by companion focus.

## Orthodoxy

`reasoning.{h,cc}` is POD, C-style, fixed buffers, no STL/exceptions/RTTI. ImGui
is confined to `reasoning_panel.cc` + `app.cc` under the existing system-include
boundary. Plain enums; pointers not references.

## Non-goals (YAGNI)

- No per-cell risk heatmap (removed before as contradictory;
  [[overlay-shows-policy-pick]]).
- No docking/multi-viewport migration.
- No menu-bar edits to the shared sibling.
- No second-window interactivity beyond drag/scroll/close and reading hover.
- No history/timeline of past moves in v1.

## Unresolved questions

None blocking. Minor items to settle during implementation:
1. Exact dot radius / halo thickness across scales — tune visually at 1× and 4×.
2. Companion default size and right-of-game placement when the game window is
   near a screen edge (clamp to display bounds).
3. Whether `exact` should also be surfaced per-cell for the hover inspect's own
   "(approx)" note, or board-level only in v1 (current plan: board-level only).
