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

struct AppState {
  SDL_Window* window;
  SDL_Renderer* renderer;
  struct Assets assets;
  struct Audio audio;
  struct Board board;
  struct Settings settings;

  int button_face; /* enum ButtonSprite */
  int press_x;     /* currently held cell, -1 if none */
  int press_y;
  bool pressing_board; /* left button held over a cell */
  bool pressing_face;  /* left button held on smiley */
  bool chord_active;   /* both buttons / middle held */

  bool timer_running;
  uint64_t timer_start_ms;
  int elapsed_sec;
  bool paused; /* minimized / unfocused */

  int pending_name_level; /* level awaiting Enter-Name, -1 if none */
  struct DialogState dialogs;

  char asset_dir[1024];
  char pref_path[1024];

  /* --- solver additions --- */
  struct Analysis analysis; /* recomputed when the revealed set changes */
  bool overlay_on;          /* F10 toggles the analysis overlay */
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
