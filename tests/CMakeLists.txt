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
