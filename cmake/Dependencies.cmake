include(FetchContent)

# ---------------------------------------------------------------------------
# Minesweeper upstream — the original game this project overlays. We compile
# selected sources into our own libraries (see src/CMakeLists.txt) rather than
# building upstream's CMake project, so we only need its populated source tree:
# SOURCE_SUBDIR names a path with no CMakeLists.txt, which makes
# FetchContent_MakeAvailable() skip add_subdirectory(). Pinned to a specific
# commit for reproducibility (no GIT_SHALLOW — shallow clones can't fetch an
# arbitrary commit SHA). Skipped entirely when MINESWEEPER_DIR already names a
# local checkout (-DMINESWEEPER_DIR=<path>).
# ---------------------------------------------------------------------------
if(NOT MINESWEEPER_DIR)
  FetchContent_Declare(
    minesweeper
    GIT_REPOSITORY https://github.com/monde-lointain/minesweeper.git
    GIT_TAG d62c0b248d90e92f62fa5078d2702877cdc31932
    SOURCE_SUBDIR _sources_only_no_cmakelists
  )
  FetchContent_MakeAvailable(minesweeper)
  set(MINESWEEPER_DIR "${minesweeper_SOURCE_DIR}")
endif()
if(NOT EXISTS "${MINESWEEPER_DIR}/src/game/game.cc")
  message(FATAL_ERROR
    "MINESWEEPER_DIR='${MINESWEEPER_DIR}' does not contain the minesweeper "
    "sources (src/game/game.cc not found). Set -DMINESWEEPER_DIR=<path> to a "
    "local checkout, or leave it empty to fetch the pinned upstream commit.")
endif()

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
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
set(SDL_X11_XTEST OFF)  # libxtst-dev not on CI runners; game uses no synthetic input
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
set(SDLMIXER_INSTALL OFF)
set(SDLMIXER_TESTS OFF)
set(SDLMIXER_SAMPLES OFF)
set(SDLMIXER_EXAMPLES OFF)
set(SDLMIXER_VENDORED OFF)
set(SDLMIXER_WAVE ON)
set(SDLMIXER_FLAC OFF)
set(SDLMIXER_MP3 OFF)
set(SDLMIXER_MOD OFF)
set(SDLMIXER_MIDI OFF)
set(SDLMIXER_OPUS OFF)
set(SDLMIXER_VORBIS_STB OFF)
set(SDLMIXER_VORBIS_VORBISFILE OFF)
set(SDLMIXER_VORBIS_TREMOR OFF)
set(SDLMIXER_GME OFF)
set(SDLMIXER_WAVPACK OFF)
set(SDLMIXER_AIFF OFF)
set(SDLMIXER_VOC OFF)
set(SDLMIXER_AU OFF)
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
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
    SYSTEM
  )
  # gtest defaults to the static /MT runtime on MSVC; the project uses the
  # dynamic /MD runtime, so force the shared CRT to avoid LNK2038 when the
  # SDL-free test exes link gtest.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
