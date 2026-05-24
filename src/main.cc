/* main.cc — SDL3 callback entry (Stream D, forked from the sibling). Defines
 * the SDL_MAIN_USE_CALLBACKS hooks and delegates to app.cc. */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "solver/app.h"

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
  struct AppState *s = NULL;
  SDL_AppResult r = app_init(&s, argc, argv);
  *appstate = s;
  return r;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  return app_event((struct AppState *)appstate, event);
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  return app_iterate((struct AppState *)appstate);
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  (void)result;
  app_quit((struct AppState *)appstate);
}
