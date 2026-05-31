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
target_compile_options(minesweeper_reuse PRIVATE -Wall -Wextra)

# Engine + pure overlay helpers — no SDL, so engine_test/overlay_test link these
# without a graphics stack.
add_library(solver_lib STATIC
  engine/engine.cc
  overlay/overlay_geom.cc
)
target_include_directories(solver_lib PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${MINESWEEPER_DIR}/include
)
set(SOLVER_WARNINGS -Wall -Wextra -Wpedantic)
target_compile_options(solver_lib PRIVATE ${SOLVER_WARNINGS})

# Executable: forked app + overlay draw + entry, on top of the reused game.
add_executable(minesweeper_solver
  main.cc
  app/app.cc
  overlay/overlay.cc
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
