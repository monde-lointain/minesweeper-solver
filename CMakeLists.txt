cmake_minimum_required(VERSION 3.28)

# Compiler choice (clang-22, required for the Orthodoxy plugin — built for clang
# major 22) lives in CMakePresets.json (`cmake --preset dev`) and the Makefile,
# NOT here. Building with a stock clang silently disables enforcement;
# Orthodoxy.cmake reports a STATUS line when the plugin is not found.
project(minesweeper_solver VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options.
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MINESWEEPER_SOLVER_WERROR "Treat warnings as errors for project targets" ON)

# Minesweeper upstream sources we reuse (game/render/assets/ui/config/audio/util
# + the original BMP/WAV assets). Empty (the default) fetches the pinned upstream
# commit; set to a local checkout to build against working sources instead.
# Resolved (fetched when empty) and validated in cmake/Dependencies.cmake.
set(MINESWEEPER_DIR "" CACHE PATH
    "Path to a local minesweeper checkout (empty = fetch the pinned upstream commit)")

if(ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "Sanitizers enabled")
  endif()
endif()

include(cmake/Dependencies.cmake)
include(cmake/Orthodoxy.cmake)
include(cmake/StaticAnalysis.cmake)
include(cmake/Formatting.cmake)

add_subdirectory(src)

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
