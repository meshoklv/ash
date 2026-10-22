if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)   # standalone: psp-cmake -S src/boss -B build/boss-psp
  cmake_minimum_required(VERSION 3.16)
  project(ash_boss C)
  set(CMAKE_C_STANDARD 99)
  add_compile_options(-O2 -G0 -Wall)
endif()
# Asylum Demon runtime (boss.c) + the boss test EBOOT (bosstest.c: arena, scripted stand-in player, screenshots).
# Uses src/game's anim/chr/scene code directly (no library there yet).
set(GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../game)
add_library(ash_boss STATIC boss.c ${GAME_DIR}/anim.c ${GAME_DIR}/chr.c)
target_include_directories(ash_boss PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${GAME_DIR})

add_executable(ash_bosstest bosstest.c ${GAME_DIR}/scene.c ${GAME_DIR}/pcol.c)
target_link_libraries(ash_bosstest PRIVATE ash_boss pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)
create_pbp_file(TARGET ash_bosstest TITLE "Ash Boss" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Lua mode (original 223200_battle on the src/ai host): only when src/ai built (needs ~/opt/lua50)
if(NOT TARGET ash_ai)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ai ${CMAKE_CURRENT_BINARY_DIR}/ai)
endif()
if(TARGET ash_ai)
  target_sources(ash_bosstest PRIVATE boss_ai.c)
  target_compile_definitions(ash_bosstest PRIVATE BOSS_LUA=1)
  target_link_libraries(ash_bosstest PRIVATE ash_ai)
endif()
