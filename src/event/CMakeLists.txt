if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)   # standalone: psp-cmake -S src/event -B build/event-psp
  cmake_minimum_required(VERSION 3.16)
  project(ash_event C)
  set(CMAKE_C_STANDARD 99)
  add_compile_options(-O2 -G0 -Wall)
endif()
# Event interpreter (EMEVD -> .ashevt, tools/cook_events.py). Plain C99, no PSP calls: the game links ash_event.
add_library(ash_event STATIC event.c)
target_include_directories(ash_event PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_event PUBLIC m)

# Walkthrough test of m18_01 (same source as the native scripts/test-events.sh)
add_executable(ash_evtest test/evtest.c)
target_link_libraries(ash_evtest PRIVATE ash_event)
create_pbp_file(TARGET ash_evtest TITLE "Ash event test" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
