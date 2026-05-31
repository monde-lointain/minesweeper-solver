/* app.cc — application glue (Stream D, forked from the sibling minesweeper).
 *
 * The original SDL3-callback loop, verbatim, plus five plugin deltas:
 *   (1) a time-seeded RNG injected into game_reset (the sibling's NULL fallback
 *       replays one fixed board sequence every launch);
 *   (2) a separate pref file (winmine-solver.ini) so the original game's saved
 *       settings/best-times are never clobbered;
 *   (3) solver_analyze recomputed when the revealed set changes;
 *   (4) overlay_draw between render_frame and ImGui::Render;
 *   (5) F10 toggles the overlay.
 * Everything else (menus, dialogs, sounds, settings I/O) is unchanged.
 *
 * ImGui boundary: reference-returning ImGui calls are captured as pointers
 * (taking the address declares no C++ reference, so Orthodoxy is satisfied).
 */
#include "solver/app.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minesweeper/audio.h"
#include "minesweeper/config.h"
#include "solver/engine.h"
#include "solver/overlay.h"

/* ---- transient input state (single window) ----------------------------- */
static bool g_left_down;
static bool g_right_down;
static bool g_chorded; /* a chord fired; suppress the partner button's action */
static bool g_want_quit;
static int g_menu_bar_h; /* last ImGui main-menu-bar height in px */
static uint64_t g_pause_started_ms;

/* (1) injected RNG so boards vary across launches. */
static uint32_t g_rng_state;
static uint32_t solver_rng(void* ctx, uint32_t n) {
  (void)ctx;
  uint32_t x = g_rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_rng_state = x;
  return (n != 0u) ? (x % n) : 0u;
}

/* (3) recompute the cached analysis. */
static void app_reanalyze(struct AppState* s) {
  solver_analyze(&s->board, &s->analysis);
}

/* ---- difficulty geometry ----------------------------------------------- */
static void app_dims(const struct Settings* s, int* w, int* h, int* mines) {
  switch (s->difficulty) {
    case DIFF_INTERMEDIATE:
      *w = 16;
      *h = 16;
      *mines = 40;
      break;
    case DIFF_EXPERT:
      *w = 30;
      *h = 16;
      *mines = 99;
      break;
    case DIFF_CUSTOM:
      *w = s->custom_w;
      *h = s->custom_h;
      *mines = s->custom_mines;
      break;
    case DIFF_BEGINNER:
    default:
      *w = 9;
      *h = 9;
      *mines = 10;
      break;
  }
}

static void app_compute_layout(struct AppState* s, struct Layout* lay) {
  int mbh = s->settings.menu_visible ? g_menu_bar_h : 0;
  render_compute_layout(&s->board, &s->settings, mbh, lay);
}

/* Resize the window to fit the current board/scale/menu state. */
static void app_resize(struct AppState* s) {
  struct Layout lay;
  int cur_w = 0;
  int cur_h = 0;
  app_compute_layout(s, &lay);
  SDL_GetWindowSize(s->window, &cur_w, &cur_h);
  if (cur_w != lay.window_w || cur_h != lay.window_h) {
    SDL_SetWindowSize(s->window, lay.window_w, lay.window_h);
  }
}

static void app_new_game(struct AppState* s) {
  int w = 0;
  int h = 0;
  int mines = 0;
  app_dims(&s->settings, &w, &h, &mines);
  game_reset(&s->board, w, h, mines, solver_rng, NULL); /* (1) */
  s->button_face = BTN_HAPPY;
  s->press_x = -1;
  s->press_y = -1;
  s->pressing_board = false;
  s->pressing_face = false;
  s->chord_active = false;
  s->timer_running = false;
  s->elapsed_sec = 0;
  s->paused = false;
  g_left_down = false;
  g_right_down = false;
  g_chorded = false;
  app_resize(s);
  app_reanalyze(s); /* (3) */
}

/* Level index for best-times (only B/I/E qualify). -1 for Custom. */
static int app_level_index(const struct Settings* s) {
  if (s->difficulty == DIFF_BEGINNER) {
    return 0;
  }
  if (s->difficulty == DIFF_INTERMEDIATE) {
    return 1;
  }
  if (s->difficulty == DIFF_EXPERT) {
    return 2;
  }
  return -1;
}

/* React to a reveal/chord result: sounds, face, best-time prompt. */
static void app_after_action(struct AppState* s, int result) {
  if (result == REVEAL_LOSS) {
    s->button_face = BTN_LOSE;
    s->timer_running = false;
    audio_play_explode(s->settings.sound);
  } else if (result == REVEAL_WIN) {
    s->button_face = BTN_WIN;
    s->timer_running = false;
    audio_play_win(s->settings.sound);
    int lvl = app_level_index(&s->settings);
    if (lvl >= 0 && s->elapsed_sec < s->settings.best_time[lvl]) {
      s->pending_name_level = lvl;
      s->show_name = true;
    }
  }
}

/* Start the timer on the first reveal of a game. */
static void app_start_timer(struct AppState* s) {
  if (!s->timer_running && s->board.status == GAME_PLAYING) {
    s->timer_running = true;
    s->timer_start_ms = SDL_GetTicks();
    s->elapsed_sec = 0;
  }
}

/* ---- init -------------------------------------------------------------- */
SDL_AppResult app_init(struct AppState** out, int argc, char** argv) {
  (void)argc;
  (void)argv;
  struct AppState* s = (struct AppState*)calloc(1, sizeof *s);
  if (s == NULL) {
    return SDL_APP_FAILURE;
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    free(s);
    return SDL_APP_FAILURE;
  }

  /* (1) seed the RNG from a high-resolution counter. */
  g_rng_state = (uint32_t)SDL_GetPerformanceCounter();
  if (g_rng_state == 0u) {
    g_rng_state = 0x1u;
  }

  if (!SDL_CreateWindowAndRenderer("Minesweeper Solver", 320, 240, 0,
                                   &s->window, &s->renderer)) {
    fprintf(stderr, "CreateWindowAndRenderer failed: %s\n", SDL_GetError());
    free(s);
    return SDL_APP_FAILURE;
  }
  SDL_SetRenderVSync(s->renderer, 1);

  /* Resolve asset dir (next to exe) and a SEPARATE config path (2). */
  const char* base = SDL_GetBasePath();
  if (base != NULL) {
    snprintf(s->asset_dir, sizeof s->asset_dir, "%sassets", base);
  } else {
    snprintf(s->asset_dir, sizeof s->asset_dir, "assets");
  }
  char* pref = SDL_GetPrefPath("", "winmine-solver");
  if (pref != NULL) {
    snprintf(s->pref_path, sizeof s->pref_path, "%swinmine-solver.ini", pref);
    SDL_free(pref);
  } else {
    snprintf(s->pref_path, sizeof s->pref_path, "winmine-solver.ini");
  }

  config_load(&s->settings, s->pref_path);

  if (!assets_load(&s->assets, s->renderer, s->asset_dir)) {
    fprintf(stderr, "asset load failed from %s\n", s->asset_dir);
    SDL_DestroyRenderer(s->renderer);
    SDL_DestroyWindow(s->window);
    free(s);
    return SDL_APP_FAILURE;
  }
  assets_set_color(&s->assets, s->settings.color);
  assets_set_window_icon(s->window, s->asset_dir);

  audio_init(s->asset_dir);

  /* ImGui. */
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO* io = &ImGui::GetIO();
  io->IniFilename = NULL; /* no imgui.ini */
  ImGui_ImplSDL3_InitForSDLRenderer(s->window, s->renderer);
  ImGui_ImplSDLRenderer3_Init(s->renderer);
  ui_apply_theme();

  s->pending_name_level = -1;
  s->overlay_on = true; /* (5) overlay starts on */
  app_new_game(s);

  if (s->settings.window_x >= 0 && s->settings.window_y >= 0) {
    SDL_SetWindowPosition(s->window, s->settings.window_x,
                          s->settings.window_y);
  }
  SDL_ShowWindow(s->window);

  *out = s;
  return SDL_APP_CONTINUE;
}

/* ---- input ------------------------------------------------------------- */
static bool app_playable(const struct AppState* s) {
  return s->board.status == GAME_READY || s->board.status == GAME_PLAYING;
}

static void app_press_update(struct AppState* s, float px, float py) {
  struct Layout lay;
  int cx = 0;
  int cy = 0;
  app_compute_layout(s, &lay);
  if (render_cell_at(&s->board, &lay, px, py, &cx, &cy)) {
    s->press_x = cx;
    s->press_y = cy;
  } else {
    s->press_x = -1;
    s->press_y = -1;
  }
}

static void app_mouse_down(struct AppState* s, const SDL_Event* e) {
  struct Layout lay;
  int cx = 0;
  int cy = 0;
  bool on_cell;
  app_compute_layout(s, &lay);
  on_cell = render_cell_at(&s->board, &lay, e->button.x, e->button.y, &cx, &cy);

  if (e->button.button == SDL_BUTTON_LEFT) {
    g_left_down = true;
    if (render_button_at(&lay, e->button.x, e->button.y)) {
      s->pressing_face = true;
      s->button_face = BTN_DOWN;
      return;
    }
  } else if (e->button.button == SDL_BUTTON_RIGHT) {
    g_right_down = true;
  }

  if (!app_playable(s)) {
    return;
  }

  /* Both buttons (or middle) -> chord intent. */
  if ((g_left_down && g_right_down) || e->button.button == SDL_BUTTON_MIDDLE) {
    s->chord_active = true;
    if (on_cell) {
      s->press_x = cx;
      s->press_y = cy;
    }
    s->button_face = BTN_CAUTION;
    return;
  }

  if (e->button.button == SDL_BUTTON_LEFT && on_cell) {
    s->pressing_board = true;
    s->press_x = cx;
    s->press_y = cy;
    s->button_face = BTN_CAUTION;
  } else if (e->button.button == SDL_BUTTON_RIGHT && on_cell) {
    game_cycle_flag(&s->board, cx, cy, s->settings.marks);
  }
}

static void app_mouse_up(struct AppState* s, const SDL_Event* e) {
  struct Layout lay;
  int cx = 0;
  int cy = 0;
  bool on_cell;
  int result = REVEAL_NONE;
  app_compute_layout(s, &lay);
  on_cell = render_cell_at(&s->board, &lay, e->button.x, e->button.y, &cx, &cy);

  /* Smiley release. */
  if (e->button.button == SDL_BUTTON_LEFT && s->pressing_face) {
    s->pressing_face = false;
    if (render_button_at(&lay, e->button.x, e->button.y)) {
      app_new_game(s);
    } else {
      s->button_face = BTN_HAPPY;
    }
    g_left_down = false;
    return;
  }

  if (s->chord_active) {
    if (on_cell && app_playable(s)) {
      result = game_chord(&s->board, cx, cy);
      app_start_timer(s);
      app_after_action(s, result);
      app_reanalyze(s); /* (3) */
    }
    s->chord_active = false;
    g_chorded = true;
    s->press_x = -1;
    s->press_y = -1;
  } else if (e->button.button == SDL_BUTTON_LEFT && s->pressing_board) {
    if (on_cell && app_playable(s) && !g_chorded) {
      result = game_reveal(&s->board, cx, cy);
      app_start_timer(s);
      app_after_action(s, result);
      app_reanalyze(s); /* (3) */
    }
    s->pressing_board = false;
    s->press_x = -1;
    s->press_y = -1;
  }

  if (e->button.button == SDL_BUTTON_LEFT) {
    g_left_down = false;
  } else if (e->button.button == SDL_BUTTON_RIGHT) {
    g_right_down = false;
  }
  if (!g_left_down && !g_right_down) {
    g_chorded = false;
  }
  if (s->button_face == BTN_CAUTION && app_playable(s)) {
    s->button_face = BTN_HAPPY;
  }
}

static void app_set_paused(struct AppState* s, bool paused) {
  if (paused == s->paused) {
    return;
  }
  if (paused) {
    g_pause_started_ms = SDL_GetTicks();
  } else if (s->timer_running) {
    s->timer_start_ms += SDL_GetTicks() - g_pause_started_ms;
  }
  s->paused = paused;
}

SDL_AppResult app_event(struct AppState* s, SDL_Event* event) {
  ImGui_ImplSDL3_ProcessEvent(event);
  ImGuiIO* io = &ImGui::GetIO();

  switch (event->type) {
    case SDL_EVENT_QUIT:
      return SDL_APP_SUCCESS;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      app_set_paused(s, true);
      break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      app_set_paused(s, false);
      break;
    case SDL_EVENT_KEY_DOWN:
      if (!io->WantCaptureKeyboard) {
        if (event->key.key == SDLK_F2) {
          app_new_game(s);
        } else if (event->key.key == SDLK_F5) {
          s->settings.menu_visible = false;
          app_resize(s);
        } else if (event->key.key == SDLK_F6) {
          s->settings.menu_visible = true;
          app_resize(s);
        } else if (event->key.key == SDLK_F10) {
          s->overlay_on = !s->overlay_on; /* (5) */
        }
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (!io->WantCaptureMouse) {
        app_mouse_down(s, event);
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (!io->WantCaptureMouse) {
        app_mouse_up(s, event);
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (!io->WantCaptureMouse && (s->pressing_board || s->chord_active)) {
        app_press_update(s, event->motion.x, event->motion.y);
      }
      break;
    default:
      break;
  }
  return SDL_APP_CONTINUE;
}

/* ---- per-frame actions ------------------------------------------------- */
static void app_apply_actions(struct AppState* s, const struct UiActions* a) {
  if (a->quit) {
    g_want_quit = true;
  }
  if (a->new_game) {
    app_new_game(s);
  }
  if (a->set_difficulty >= 0) {
    s->settings.difficulty = a->set_difficulty;
    app_new_game(s);
  }
  if (a->open_custom) {
    s->show_custom = true;
  }
  if (a->open_best) {
    s->show_best = true;
  }
  if (a->open_about) {
    s->show_about = true;
  }
  if (a->toggle_marks) {
    s->settings.marks = !s->settings.marks;
  }
  if (a->toggle_color) {
    s->settings.color = !s->settings.color;
    assets_set_color(&s->assets, s->settings.color);
  }
  if (a->toggle_sound) {
    s->settings.sound = !s->settings.sound;
  }
  if (a->set_scale > 0) {
    s->settings.scale = a->set_scale;
    app_resize(s);
  }
  if (a->toggle_menu) {
    s->settings.menu_visible = !s->settings.menu_visible;
    app_resize(s);
  }
  if (a->custom_applied) {
    s->settings.custom_w = a->custom_w;
    s->settings.custom_h = a->custom_h;
    s->settings.custom_mines = a->custom_mines;
    s->settings.difficulty = DIFF_CUSTOM;
    app_new_game(s);
  }
  if (a->best_reset) {
    for (int i = 0; i < LEVEL_COUNT; ++i) {
      s->settings.best_time[i] = 999;
      strncpy(s->settings.best_name[i], "Anonymous", SCORE_NAME_MAX - 1);
      s->settings.best_name[i][SCORE_NAME_MAX - 1] = '\0';
    }
    config_save(&s->settings, s->pref_path);
  }
  if (a->name_entered && s->pending_name_level >= 0) {
    int lvl = s->pending_name_level;
    s->settings.best_time[lvl] = s->elapsed_sec;
    snprintf(s->settings.best_name[lvl], SCORE_NAME_MAX, "%s", a->name);
    s->pending_name_level = -1;
    config_save(&s->settings, s->pref_path);
  }
}

SDL_AppResult app_iterate(struct AppState* s) {
  struct Layout lay;
  struct UiActions actions;
  float menu_h;

  /* Advance timer. */
  if (s->timer_running && !s->paused) {
    uint64_t now = SDL_GetTicks();
    int sec = (int)((now - s->timer_start_ms) / 1000);
    s->elapsed_sec = (sec > 999) ? 999 : sec;
  }

  /* ImGui frame. */
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  ui_actions_clear(&actions);
  menu_h = ui_menu_bar(&s->settings, &actions);
  g_menu_bar_h = (int)menu_h;
  ui_dialogs(&s->settings, &actions, &s->show_custom, &s->show_best,
             &s->show_about, &s->show_name, s->pending_name_level);
  app_apply_actions(s, &actions);

  /* Keep window sized to the (possibly changed) menu-bar height. */
  app_resize(s);
  app_compute_layout(s, &lay);

  /* Resolve smiley face for non-pressing states. */
  if (!s->pressing_face && !s->pressing_board && !s->chord_active) {
    if (s->board.status == GAME_LOST) {
      s->button_face = BTN_LOSE;
    } else if (s->board.status == GAME_WON) {
      s->button_face = BTN_WIN;
    } else {
      s->button_face = BTN_HAPPY;
    }
  }

  /* Draw: board+chrome, then the analysis overlay (4), then ImGui on top. */
  SDL_SetRenderDrawColor(s->renderer, 192, 192, 192, 255);
  SDL_RenderClear(s->renderer);
  struct FrameView view;
  view.button_face = s->button_face;
  view.press_x = s->press_x;
  view.press_y = s->press_y;
  view.elapsed_sec = s->elapsed_sec;
  render_frame(s->renderer, &s->assets, &s->board, &lay, &view);
  if (s->overlay_on) {
    overlay_draw(&s->analysis, &s->board, &lay);
  }
  ImGui::Render();
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s->renderer);
  SDL_RenderPresent(s->renderer);

  return g_want_quit ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

void app_quit(struct AppState* s) {
  if (s == NULL) {
    return;
  }
  /* Persist window position + settings. */
  int wx = 0;
  int wy = 0;
  SDL_GetWindowPosition(s->window, &wx, &wy);
  s->settings.window_x = wx;
  s->settings.window_y = wy;
  config_save(&s->settings, s->pref_path);

  audio_shutdown();
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  assets_free(&s->assets);
  if (s->renderer != NULL) {
    SDL_DestroyRenderer(s->renderer);
  }
  if (s->window != NULL) {
    SDL_DestroyWindow(s->window);
  }
  free(s);
}
