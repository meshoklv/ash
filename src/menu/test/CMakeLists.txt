# Stand-alone test EBOOT for src/menu (autopilot through every screen, frame dumps). Own build dir:
#   . scripts/env.sh && psp-cmake -S src/menu/test -B build/menu -G Ninja -DCMAKE_BUILD_TYPE=Release
#   nice -n 19 cmake --build build/menu          # -> build/menu/EBOOT.PBP;  run: scripts in docs/menu.md
cmake_minimum_required(VERSION 3.16)
project(ash_menutest C)
if(NOT PSP)
  message(FATAL_ERROR "Configure with psp-cmake (pspdev toolchain)")
endif()
set(CMAKE_C_STANDARD 99)
add_compile_options(-O2 -G0 -Wall)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_executable(ash_menutest main.c
  ${SRC}/menu/menu.c ${SRC}/menu/menu_create.c ${SRC}/menu/menu_game.c
  ${SRC}/ui/ui.c ${SRC}/game/chr.c ${SRC}/game/anim.c ${SRC}/game/scene.c)
target_include_directories(ash_menutest PRIVATE ${SRC}/menu ${SRC}/ui ${SRC}/game)
target_link_libraries(ash_menutest PRIVATE pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)
create_pbp_file(
  TARGET ash_menutest
  TITLE "Ash Menu"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
