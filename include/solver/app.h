/* app.h — solver application state + SDL3 callback contract (Stream D).
 *
 * Forked from the sibling minesweeper app.h: the full original AppState plus a
 * cached engine Analysis and the overlay on/off toggle. main.cc defines the
 * SDL_MAIN_USE_CALLBACKS entry points and delegates to app.cc.
 */
#ifndef SOLVER_APP_H
#define SOLVER_APP_H

#include <SDL3/SDL.h>
#include <stdint.h>

#include "minesweeper/assets.h"
#include "minesweeper/audio.h"
#include "minesweeper/game.h"
#include "minesweeper/render.h"
#include "minesweeper/types.h"
#include "minesweeper/ui.h"
#include "solver/engine.h"
#include "solver/geom.h"

/* Board fields the cached analysis depends on. When this tuple changes the
 * analysis is recomputed; flags are deliberately excluded (the engine ignores
 * them). */
struct AnalysisKey {
  int status;
  int revealed_count;
  int width;
  int height;
  int mines;
};

struct AppState {
  SDL_Window* window;
  SDL_Renderer* renderer;
  struct Assets assets;
  struct Audio audio;
  struct Board board;
  struct Settings settings;

  int button_face;     /* enum ButtonSprite */
  struct Pt press;     /* currently held cell, press.x < 0 if none */
  bool pressing_board; /* left button held over a cell */
  bool pressing_face;  /* left button held on smiley */
  bool chord_active;   /* both buttons / middle held */

  /* transient input + app state (formerly file-scope globals) */
  bool left_down;
  bool right_down;
  bool chorded; /* a chord fired; suppress the partner button's action */
  bool want_quit;
  int menu_bar_h;            /* last ImGui main-menu-bar height in px */
  uint64_t pause_started_ms; /* SDL_GetTicks() when pause began */
  uint32_t rng_state;        /* xorshift state for solver_rng (via Rng.ctx) */

  bool timer_running;
  uint64_t timer_start_ms;
  int elapsed_sec;
  bool paused; /* minimized / unfocused */

  int pending_name_level; /* level awaiting Enter-Name, -1 if none */
  struct DialogState dialogs;

  char asset_dir[1024];
  char pref_path[1024];

  /* --- solver additions --- */
  struct SolverScratch* scratch; /* engine working memory (per-app instance) */
  struct Analysis analysis;      /* recomputed when analysis_key changes */
  struct AnalysisKey analysis_key; /* last-analyzed board signature */
  bool overlay_on;                 /* F10 toggles the analysis overlay */

  /* --- companion reasoning window --- */
  SDL_Window* panel_window;
  SDL_Renderer* panel_renderer;
  void* ctx_game;  /* ImGuiContext* (opaque here to avoid an ImGui include) */
  void* ctx_panel; /* ImGuiContext* */
  bool panel_on;   /* F9 toggles the companion window */
  struct Pt hover; /* covered cell under the game cursor, hover.x < 0 if none */
};

/* Allocate + initialize: SDL, window/renderer, ImGui, config, assets, first
 * game + analysis. Returns SDL_APP_CONTINUE on success. */
SDL_AppResult app_init(struct AppState** out, int argc, char** argv);

/* One event. */
SDL_AppResult app_event(struct AppState* s, SDL_Event* event);

/* One frame (continuous vsync redraw). */
SDL_AppResult app_iterate(struct AppState* s);

/* Teardown + save settings. */
void app_quit(struct AppState* s);

#endif /* SOLVER_APP_H */
