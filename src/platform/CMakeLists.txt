# The platform primitives: one interface each, with an Apple and a Windows implementation. This file is also a
# project of its own (cmake -S src/platform), so the primitives and their tests build where the rest does not yet.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.24)
  project(nocturne_platform LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(platform_standalone ON)
endif()

set(repo_root ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_library(nocturne_platform STATIC)
if(APPLE)
  target_sources(nocturne_platform PRIVATE
    clock_apple.cpp
  )
elseif(WIN32)
  target_sources(nocturne_platform PRIVATE
    clock_win.cpp
  )
  target_compile_definitions(nocturne_platform PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
target_include_directories(nocturne_platform PUBLIC ${repo_root}/src)

# Built into nocturne_tests as well.
set(platform_tests
  ${repo_root}/tests/platform_clock_test.cpp
)

if(NOT platform_standalone)
  set(NOCTURNE_PLATFORM_TESTS ${platform_tests} PARENT_SCOPE)
else()
  enable_testing()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/platform_tests_main.cpp
    "#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN\n#include <doctest.h>\n")
  add_executable(nocturne_platform_tests ${CMAKE_CURRENT_BINARY_DIR}/platform_tests_main.cpp ${platform_tests})
  target_include_directories(nocturne_platform_tests PRIVATE ${repo_root}/tests ${repo_root}/third_party/doctest)
  target_link_libraries(nocturne_platform_tests PRIVATE nocturne_platform)
  add_test(NAME nocturne_platform_tests COMMAND nocturne_platform_tests)
endif()
