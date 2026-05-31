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
