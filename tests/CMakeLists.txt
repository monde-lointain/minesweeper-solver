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

# Benchmark harness tests. Each links bench_lib (which PUBLIC-propagates the
# bench include dir + solver_lib/game_core). Per-module surfaces keep ownership
# file-disjoint across streams; bench_test is the cross-module integration gate.
add_executable(bench_policy_test bench_policy_test.cc)
target_link_libraries(bench_policy_test PRIVATE bench_lib gtest_main)
gtest_discover_tests(bench_policy_test)

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
