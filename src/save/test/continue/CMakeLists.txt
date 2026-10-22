# "Продолжить" demo EBOOT: src/menu + src/save across emulator restarts (scripts/test-save-continue.sh). Own build dir:
#   . scripts/env.sh && psp-cmake -S src/save/test/continue -B build/savecont -G Ninja -DCMAKE_BUILD_TYPE=Release
#   nice -n 19 cmake --build build/savecont
cmake_minimum_required(VERSION 3.16)
project(ash_savecont C)
if(NOT PSP)
  message(FATAL_ERROR "Configure with psp-cmake (pspdev toolchain)")
endif()
set(CMAKE_C_STANDARD 99)
add_compile_options(-O2 -G0 -Wall)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_executable(ash_savecont main.c
  ${SRC}/save/savegame.c ${SRC}/save/savestore.c ${SRC}/progress/progress.c ${SRC}/progress/save.c
  ${SRC}/menu/menu.c ${SRC}/menu/menu_create.c ${SRC}/menu/menu_game.c
  ${SRC}/ui/ui.c ${SRC}/game/chr.c ${SRC}/game/anim.c ${SRC}/game/scene.c)
target_include_directories(ash_savecont PRIVATE ${SRC}/save ${SRC}/progress ${SRC}/menu ${SRC}/ui ${SRC}/game)
target_link_libraries(ash_savecont PRIVATE pspgum pspgu pspge pspdisplay pspctrl psputility psprtc pspdebug m)
create_pbp_file(
  TARGET ash_savecont
  TITLE "Ash save continue"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
