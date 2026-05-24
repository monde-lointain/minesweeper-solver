include(FetchContent)

# ---------------------------------------------------------------------------
# SDL3 — must be made available BEFORE SDL_mixer (SDL_mixer reuses the in-tree
# SDL3::SDL3 target only if it already exists).
# ---------------------------------------------------------------------------
FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.8
  GIT_SHALLOW TRUE
  SYSTEM
)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

# ---------------------------------------------------------------------------
# SDL_mixer — WAV only; disable every other decoder so no codec submodules or
# system libraries are required.
# ---------------------------------------------------------------------------
FetchContent_Declare(
  SDL3_mixer
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
  GIT_TAG release-3.2.2
  GIT_SHALLOW TRUE
  SYSTEM
)
set(SDLMIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_WAVE ON CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MP3 OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDLMIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_STB OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_TREMOR OFF CACHE BOOL "" FORCE)
set(SDLMIXER_GME OFF CACHE BOOL "" FORCE)
set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)
set(SDLMIXER_AIFF OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VOC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_AU OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3_mixer)

# ---------------------------------------------------------------------------
# Dear ImGui — ships no CMake target; build a static lib of core + the SDL3 /
# SDLRenderer3 backends ourselves. Headers exposed SYSTEM so the Orthodoxy
# plugin ignores ImGui declarations pulled into our TUs.
# ---------------------------------------------------------------------------
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.8
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# ---------------------------------------------------------------------------
# mINI — header-only INI parser. INTERFACE target, SYSTEM include.
# ---------------------------------------------------------------------------
FetchContent_Declare(
  mini
  GIT_REPOSITORY https://github.com/metayeti/mINI.git
  GIT_TAG 0.9.18
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(mini)

add_library(mini INTERFACE)
target_include_directories(mini SYSTEM INTERFACE ${mini_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# GoogleTest (tests only).
# ---------------------------------------------------------------------------
if(BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(googletest)
endif()
