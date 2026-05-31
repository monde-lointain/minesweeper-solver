# clang-tidy target (Orthodox-loosened .clang-tidy). Runs over OUR sources only
# (the reused sibling sources are linted in their own repo). We pass explicit
# compile flags after `--` rather than using compile_commands.json, so clang-tidy
# does NOT replay the `-fplugin=orthodoxy.so` flag (the Orthodoxy plugin is a
# clang-22 compiler plugin and crashes inside clang-tidy's tooling path; Orthodox
# enforcement already happens at build time via the compiler).
find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy-22 clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
  file(GLOB_RECURSE TIDY_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/*.cc)
  add_custom_target(tidy
    COMMAND ${CLANG_TIDY_EXECUTABLE} --quiet ${TIDY_SOURCE_FILES} --
      -std=c++20
      -I${PROJECT_SOURCE_DIR}/include
      -I${MINESWEEPER_DIR}/include
      -isystem ${sdl3_SOURCE_DIR}/include
      -isystem ${sdl3_BINARY_DIR}/include-revision
      -isystem ${sdl3_mixer_SOURCE_DIR}/include
      -isystem ${imgui_SOURCE_DIR}
      -isystem ${imgui_SOURCE_DIR}/backends
      -isystem ${mini_SOURCE_DIR}/src
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Running clang-tidy"
  )
endif()
