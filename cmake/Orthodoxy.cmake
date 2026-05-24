# Orthodoxy — Clang plugin enforcing Orthodox C++. Optional: only engages when
# building with the matching clang (clang++-22 here) and the plugin is installed.
# The `orthodoxy::plugin` INTERFACE target adds `-fplugin=...`; src/CMakeLists.txt
# links it PRIVATE to project targets only (never deps, never tests).
find_package(orthodoxy CONFIG QUIET OPTIONAL_COMPONENTS plugin)

if(orthodoxy_plugin_FOUND)
  message(STATUS "Orthodoxy plugin found — Orthodox C++ enforcement ON for project targets")
else()
  message(STATUS "Orthodoxy plugin not found — building without enforcement "
                 "(use clang++-22 with Orthodoxy installed to enable)")
endif()
