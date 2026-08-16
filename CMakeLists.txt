cmake_minimum_required(VERSION 3.16)
project(pc1500preset VERSION 0.1.0 LANGUAGES CXX)

# This project builds only its own preset-applier tool (src/). The vendored
# emulator under vendor/pc1500emu is a separate, independent project (a git
# submodule pointing at an unmodified checkout of pc1500emu) with its own
# CMake build -- it is never added as a subdirectory here, and never
# patched. Build it on its own, e.g.:
#
#   cmake -B vendor/pc1500emu/build -S vendor/pc1500emu
#   cmake --build vendor/pc1500emu/build
#
# See scripts/build_all.sh for a convenience wrapper that builds both.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# .pc1500 preset files are YAML (docs/preset_file_format.md); fetched at
# configure time rather than assumed to be installed system-wide, since
# there's no other system-package dependency in this project to match.
include(FetchContent)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
)
# yaml-cpp 0.8.0's own CMakeLists.txt predates CMake removing support for
# cmake_minimum_required() versions below 3.5 -- this only relaxes that
# check for yaml-cpp's own (unmodified, vendored-at-configure-time) build,
# not this project's.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(yaml-cpp)
unset(CMAKE_POLICY_VERSION_MINIMUM)

add_subdirectory(src)
