cmake_minimum_required(VERSION 3.21)

project(
  integra_littlefs_cpp
  VERSION 0.1.0
  LANGUAGES CXX)

# littlefs is not an integra-lib component, and has no CMake build of its own,
# so the consumer names the target that provides it: lfs.h on its include path
# and lfs.c linked. A project that already builds littlefs keeps doing so.
set(INTEGRA_LITTLEFS_TARGET
    littlefs
    CACHE STRING "CMake target providing littlefs (lfs.h and lfs.c)")

# Built on its own, the component fetches littlefs at the version it was
# verified against. A consumer never reaches this branch.
if(PROJECT_IS_TOP_LEVEL AND NOT TARGET ${INTEGRA_LITTLEFS_TARGET})
  enable_language(C)
  include(FetchContent)
  FetchContent_Declare(
    littlefs
    GIT_REPOSITORY https://github.com/littlefs-project/littlefs.git
    GIT_TAG v2.10.1)
  FetchContent_MakeAvailable(littlefs)
  add_library(${INTEGRA_LITTLEFS_TARGET} STATIC
              ${littlefs_SOURCE_DIR}/lfs.c ${littlefs_SOURCE_DIR}/lfs_util.c)
  target_include_directories(${INTEGRA_LITTLEFS_TARGET} SYSTEM
                             PUBLIC ${littlefs_SOURCE_DIR})
  # LFS_NO_MALLOC proves the wrapper never needs littlefs's allocator: the
  # allocating lfs_file_open does not exist in this build.
  target_compile_definitions(
    ${INTEGRA_LITTLEFS_TARGET} PUBLIC LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN
                                      LFS_NO_TRACE)
endif()

if(NOT TARGET ${INTEGRA_LITTLEFS_TARGET})
  message(
    FATAL_ERROR
      "littlefs_cpp needs a CMake target providing littlefs, named by INTEGRA_LITTLEFS_TARGET "
      "(now '${INTEGRA_LITTLEFS_TARGET}'). Add it before this directory.")
endif()

add_library(integra_littlefs_cpp INTERFACE)
add_library(Integra::littlefs_cpp ALIAS integra_littlefs_cpp)

target_include_directories(integra_littlefs_cpp
                           INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(integra_littlefs_cpp INTERFACE cxx_std_20)
target_link_libraries(integra_littlefs_cpp INTERFACE ${INTEGRA_LITTLEFS_TARGET})

# Dependants read the version from this property rather than from a variable: a
# variable set here is invisible in a sibling directory, and a stale one left in
# the parent scope would be read instead — a version check that silently passes.
set_target_properties(integra_littlefs_cpp PROPERTIES INTEGRA_VERSION
                                                      ${PROJECT_VERSION})

if(PROJECT_IS_TOP_LEVEL)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
  FetchContent_MakeAvailable(googletest)

  include(GoogleTest)
  enable_testing()

  add_executable(littlefs-cpp-test tests/littlefs_test.cpp)
  target_link_libraries(littlefs-cpp-test PRIVATE Integra::littlefs_cpp
                                                  GTest::gtest_main)
  # littlefs keeps pointers to open files and directories and writes through
  # them; a handle left behind on a dead stack frame is exactly what these
  # catch.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      littlefs-cpp-test PRIVATE -fsanitize=address,undefined
                                -fno-omit-frame-pointer)
    target_link_options(littlefs-cpp-test PRIVATE -fsanitize=address,undefined)
  endif()
  gtest_discover_tests(littlefs-cpp-test)
endif()
