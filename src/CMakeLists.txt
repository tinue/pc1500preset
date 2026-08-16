# pc1500preset: a standalone tool that applies a .pc1500 file to an
# unmodified pc1500emu process (vendor/pc1500emu, a git submodule) over its
# own FIFO scripting interface. See main.cpp's top comment for why this
# deliberately has no dependency on any of the emulator's own CPU/bus/host
# code.
add_executable(pc1500preset
  main.cpp
  preset_file.cpp
)
target_include_directories(pc1500preset PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pc1500preset PRIVATE yaml-cpp::yaml-cpp)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(pc1500preset PRIVATE Threads::Threads)
endif()
