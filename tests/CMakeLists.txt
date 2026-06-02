include(GoogleTest)

# Engine + overlay-helper tests link only solver_lib (no SDL) + gtest.
add_executable(engine_test engine_test.cc)
target_link_libraries(engine_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(engine_test)

add_executable(overlay_test overlay_test.cc)
target_link_libraries(overlay_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(overlay_test)

# Characterization gate for the engine refactor (covers the fallback/large-board
# paths the brute-force oracle never reaches).
add_executable(golden_test golden_test.cc)
target_link_libraries(golden_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(golden_test)

# Large-N correctness oracle (Stream A.1-alpha). solver_lib_ld recompiles the
# SAME engine sources with ereal widened to long double (ENGINE_REAL_LDBL; range
# ~1e4932, never overflows for the test boards) and its public symbols renamed
# `_ld`, so it links alongside the production double solver_lib in one test exe.
# Engine helpers (solver_neighbor, game_index) are header-inline -> self-contained.
add_library(solver_lib_ld STATIC
  ${PROJECT_SOURCE_DIR}/src/engine/engine.cc
  ${PROJECT_SOURCE_DIR}/src/engine/engine_reduce.cc)
target_include_directories(solver_lib_ld PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src/engine
  ${MINESWEEPER_DIR}/include)
target_compile_definitions(solver_lib_ld PRIVATE
  ENGINE_REAL_LDBL
  solver_analyze=solver_analyze_ld
  solver_analyze_infogain=solver_analyze_infogain_ld
  solver_scratch_create=solver_scratch_create_ld
  solver_scratch_destroy=solver_scratch_destroy_ld
  solver_test_set_force_reduce=solver_test_set_force_reduce_ld
  enumerate_reduced=enumerate_reduced_ld)

add_executable(engine_largeN_test engine_largeN_test.cc)
target_link_libraries(engine_largeN_test PRIVATE solver_lib_ld solver_lib gtest_main)
gtest_discover_tests(engine_largeN_test)

# Emulated-128-bit parity (MSVC path coverage). The production engine uses the
# native __int128; MSVC falls back to the two-limb software RatI128 (rational.h,
# RAT_FORCE_EMULATED_I128), which CI otherwise never compiles. solver_lib_emu
# recompiles the SAME engine sources with that emulation forced and its public
# symbols renamed `_emu`, so the native and emulated engines link into one exe
# and engine_emulated_test asserts they produce identical marginals.
add_library(solver_lib_emu STATIC
  ${PROJECT_SOURCE_DIR}/src/engine/engine.cc
  ${PROJECT_SOURCE_DIR}/src/engine/engine_reduce.cc)
target_include_directories(solver_lib_emu PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src/engine
  ${MINESWEEPER_DIR}/include)
target_compile_definitions(solver_lib_emu PRIVATE
  RAT_FORCE_EMULATED_I128
  solver_analyze=solver_analyze_emu
  solver_analyze_infogain=solver_analyze_infogain_emu
  solver_scratch_create=solver_scratch_create_emu
  solver_scratch_destroy=solver_scratch_destroy_emu
  solver_test_set_force_reduce=solver_test_set_force_reduce_emu
  enumerate_reduced=enumerate_reduced_emu)

add_executable(engine_emulated_test engine_emulated_test.cc)
target_link_libraries(engine_emulated_test PRIVATE solver_lib_emu solver_lib gtest_main)
target_include_directories(engine_emulated_test PRIVATE ${PROJECT_SOURCE_DIR}/src/engine)
gtest_discover_tests(engine_emulated_test)

# Engine-internal tests (exact-rational primitive + Gaussian-reduction driver).
# They reach into src/engine for rational.h / engine_internal.h, which are NOT
# part of the installed contract.
add_executable(rational_test rational_test.cc)
target_link_libraries(rational_test PRIVATE solver_lib gtest_main)
target_include_directories(rational_test PRIVATE ${PROJECT_SOURCE_DIR}/src/engine)
gtest_discover_tests(rational_test)

add_executable(engine_reduction_test engine_reduction_test.cc)
target_link_libraries(engine_reduction_test PRIVATE solver_lib gtest_main)
target_include_directories(engine_reduction_test
  PRIVATE ${PROJECT_SOURCE_DIR}/src/engine)
gtest_discover_tests(engine_reduction_test)

# Info-gain (paper's Inf(x)) vs an independent brute-force reference. Reaches into
# src/engine for engine_internal.h (the force-reduce hook) to differentially test
# the reduced info-gain path the small ASCII oracle never reaches.
add_executable(engine_infogain_test engine_infogain_test.cc)
target_link_libraries(engine_infogain_test PRIVATE solver_lib gtest_main)
target_include_directories(engine_infogain_test
  PRIVATE ${PROJECT_SOURCE_DIR}/src/engine)
gtest_discover_tests(engine_infogain_test)

# Shared move recommendation (the bench policy and the GUI overlay both call it).
add_executable(recommend_test recommend_test.cc)
target_link_libraries(recommend_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(recommend_test)

# Analysis.exact: exact-vs-fallback probability signal for the reasoning overlay.
add_executable(engine_exact_test engine_exact_test.cc)
target_link_libraries(engine_exact_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(engine_exact_test)

# Pure reasoning_build (ReasoningView extraction for the companion panel).
add_executable(reasoning_test reasoning_test.cc)
target_link_libraries(reasoning_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(reasoning_test)

# Benchmark harness tests. Each links bench_lib (which PUBLIC-propagates the
# bench include dir + solver_lib/game_core). Per-module surfaces keep ownership
# file-disjoint across streams; bench_test is the cross-module integration gate.
add_executable(bench_policy_test bench_policy_test.cc)
target_link_libraries(bench_policy_test PRIVATE bench_lib gtest_main)
gtest_discover_tests(bench_policy_test)

add_executable(bench_policy_infogain_test bench_policy_infogain_test.cc)
target_link_libraries(bench_policy_infogain_test PRIVATE bench_lib gtest_main)
gtest_discover_tests(bench_policy_infogain_test)

add_executable(bench_metrics_test bench_metrics_test.cc)
target_link_libraries(bench_metrics_test PRIVATE bench_lib gtest_main)
gtest_discover_tests(bench_metrics_test)

add_executable(bench_runner_test bench_runner_test.cc)
target_link_libraries(bench_runner_test PRIVATE bench_lib gtest_main)
gtest_discover_tests(bench_runner_test)

add_executable(bench_cli_test bench_cli_test.cc)
target_link_libraries(bench_cli_test PRIVATE bench_lib gtest_main)
gtest_discover_tests(bench_cli_test)

add_executable(bench_test bench_test.cc)
target_link_libraries(bench_test PRIVATE bench_lib gtest_main)
gtest_discover_tests(bench_test)
