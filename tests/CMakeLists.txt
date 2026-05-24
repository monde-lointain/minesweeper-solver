include(GoogleTest)

# Engine + overlay-helper tests link only solver_lib (no SDL) + gtest.
add_executable(engine_test engine_test.cc)
target_link_libraries(engine_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(engine_test)

add_executable(overlay_test overlay_test.cc)
target_link_libraries(overlay_test PRIVATE solver_lib gtest_main)
gtest_discover_tests(overlay_test)
