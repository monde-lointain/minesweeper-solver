# Per-compiler warning flags. MSVC rejects the GCC/Clang -W* spellings (it maps
# -Wall to /Wall, which is unusably noisy and, under warnings-as-errors, fails
# the build), so pick the MSVC equivalent there. SOLVER_WARNINGS is the strict
# set for our own code (compiled warnings-as-error, see below); REUSE_WARNINGS
# is the looser, warnings-only set for the reused sibling sources.
if(MSVC)
  set(SOLVER_WARNINGS /W4)
  set(REUSE_WARNINGS /W4)
else()
  set(SOLVER_WARNINGS -Wall -Wextra -Wpedantic)
  set(REUSE_WARNINGS -Wall -Wextra)
endif()

# Reused sibling modules — every module except app.cc/main.cc. Preserves the
# full original game (logic, render, assets, menus/dialogs, sounds, settings).
# Built with warnings on but NOT -Werror (reused code, possibly a different
# clang than the sibling's), and WITHOUT the Orthodoxy plugin.
add_library(minesweeper_reuse STATIC
  ${MINESWEEPER_DIR}/src/game/game.cc
  ${MINESWEEPER_DIR}/src/render/render.cc
  ${MINESWEEPER_DIR}/src/assets/assets.cc
  ${MINESWEEPER_DIR}/src/ui/ui.cc
  ${MINESWEEPER_DIR}/src/config/config.cc
  ${MINESWEEPER_DIR}/src/audio/audio.cc
  ${MINESWEEPER_DIR}/src/util/util.cc
)
target_include_directories(minesweeper_reuse PUBLIC ${MINESWEEPER_DIR}/include)
target_link_libraries(minesweeper_reuse PUBLIC
  SDL3::SDL3
  SDL3_mixer::SDL3_mixer
  imgui
  mini
)
target_compile_options(minesweeper_reuse PRIVATE ${REUSE_WARNINGS})

# Engine + pure overlay helpers — no SDL, so engine_test/overlay_test link these
# without a graphics stack.
add_library(solver_lib STATIC
  engine/engine.cc
  engine/engine_reduce.cc
  engine/recommend.cc
  overlay/overlay_geom.cc
  overlay/reasoning.cc
)
target_include_directories(solver_lib PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${MINESWEEPER_DIR}/include
)
target_compile_options(solver_lib PRIVATE ${SOLVER_WARNINGS})

# Executable: forked app + overlay draw + entry, on top of the reused game.
add_executable(minesweeper_solver
  main.cc
  app/app.cc
  overlay/overlay.cc
  overlay/reasoning_panel.cc
)
target_include_directories(minesweeper_solver PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${MINESWEEPER_DIR}/include
)
target_link_libraries(minesweeper_solver PRIVATE solver_lib minesweeper_reuse)
target_compile_options(minesweeper_solver PRIVATE ${SOLVER_WARNINGS})

# Warnings-as-errors as a toggleable property (default ON) rather than a baked-in
# -Werror flag — devs/IDEs/CI can flip MINESWEEPER_SOLVER_WERROR or pass
# --compile-no-warning-as-error. minesweeper_reuse stays warnings-only.
set_target_properties(solver_lib minesweeper_solver PROPERTIES
  COMPILE_WARNING_AS_ERROR ${MINESWEEPER_SOLVER_WERROR})

# MSVC flags ISO C functions (strncpy/snprintf/getenv/...) via C4996, steering to
# its non-portable _s variants. This code is intentionally C-style; opt out so
# /W4 + warnings-as-errors keeps flagging real warnings, not deprecations.
if(MSVC)
  target_compile_definitions(solver_lib PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_definitions(minesweeper_solver PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Orthodox C++ enforcement: our targets only (never deps, never reused code).
if(orthodoxy_plugin_FOUND)
  target_link_libraries(solver_lib PRIVATE orthodoxy::plugin)
  target_link_libraries(minesweeper_solver PRIVATE orthodoxy::plugin)
endif()

# Install the original assets next to the exe so SDL_GetBasePath()+"assets/"
# resolves (reused from the sibling).
add_custom_command(TARGET minesweeper_solver POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${MINESWEEPER_DIR}/assets
    $<TARGET_FILE_DIR:minesweeper_solver>/assets
  COMMENT "Copying assets next to executable"
)

# ---------------------------------------------------------------------------
# Headless benchmark harness (Stream A scaffold). SDL-free: links only the pure
# game logic + the SDL-free engine, so it builds/runs without a graphics stack.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# SDL-free pure game logic (game + util only) — distinct from minesweeper_reuse,
# which drags in SDL via render/assets/ui/audio. Reused sibling code: warnings
# only, no Orthodoxy.
add_library(game_core STATIC
  ${MINESWEEPER_DIR}/src/game/game.cc
  ${MINESWEEPER_DIR}/src/util/util.cc
)
target_include_directories(game_core PUBLIC ${MINESWEEPER_DIR}/include)
target_compile_options(game_core PRIVATE ${REUSE_WARNINGS})

# Bench logic library (our code → Orthodoxy + warnings-as-error). std::thread /
# std::atomic in runner.cc are a sanctioned, file-local exception.
add_library(bench_lib STATIC
  bench/policy.cc
  bench/metrics.cc
  bench/runner.cc
  bench/args.cc
)
target_include_directories(bench_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bench)
target_link_libraries(bench_lib PUBLIC solver_lib game_core Threads::Threads)
target_compile_options(bench_lib PRIVATE ${SOLVER_WARNINGS})

add_executable(minesweeper_bench bench/main.cc)
target_link_libraries(minesweeper_bench PRIVATE bench_lib)
target_compile_options(minesweeper_bench PRIVATE ${SOLVER_WARNINGS})

set_target_properties(bench_lib minesweeper_bench PROPERTIES
  COMPILE_WARNING_AS_ERROR ${MINESWEEPER_SOLVER_WERROR})

if(MSVC)
  target_compile_definitions(bench_lib PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_definitions(minesweeper_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(orthodoxy_plugin_FOUND)
  target_link_libraries(bench_lib PRIVATE orthodoxy::plugin)
  target_link_libraries(minesweeper_bench PRIVATE orthodoxy::plugin)
endif()

# ---------------------------------------------------------------------------
# Profiling runner. A tiny SDL-free main() that replays the pathological run
# (100x100, 2500 mines, seed 7, info-gain) bounded by a move cap — the dedicated
# target we point valgrind/callgrind at (see `make profile`). Links the same
# pure libs as the bench, no threads.
# ---------------------------------------------------------------------------
add_executable(minesweeper_profiler profiler/main.cc)
target_include_directories(minesweeper_profiler PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${MINESWEEPER_DIR}/include
)
target_link_libraries(minesweeper_profiler PRIVATE solver_lib game_core)
target_compile_options(minesweeper_profiler PRIVATE ${SOLVER_WARNINGS})

set_target_properties(minesweeper_profiler PROPERTIES
  COMPILE_WARNING_AS_ERROR ${MINESWEEPER_SOLVER_WERROR})

if(MSVC)
  target_compile_definitions(minesweeper_profiler PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(orthodoxy_plugin_FOUND)
  target_link_libraries(minesweeper_profiler PRIVATE orthodoxy::plugin)
endif()
