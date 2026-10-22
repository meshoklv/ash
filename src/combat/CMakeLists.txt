# Combat rules (DS1) on cooked TAE/PARAM tables. Plain C99, no PSP headers: also built on the PC by tests/combat/run.sh.
add_library(ash_combat STATIC combat.c cbdata.c)
target_include_directories(ash_combat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Combat test EBOOT (src/combat/test/cbtest.c): knight vs hollow / Asylum Demon on the arena, scripted autopilot
# (block + repel, parry -> riposte, backstab, running + heavy attack, player poise, plunge). Uses src/game's
# anim/chr/scene/pcol sources directly (as src/boss does); run: src/combat/test/shoot-cbtest.sh
set(ASH_GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../game)
add_executable(ash_cbtest test/cbtest.c ${ASH_GAME_DIR}/anim.c ${ASH_GAME_DIR}/chr.c ${ASH_GAME_DIR}/scene.c
               ${ASH_GAME_DIR}/pcol.c)
target_include_directories(ash_cbtest PRIVATE ${ASH_GAME_DIR})
target_link_libraries(ash_cbtest PRIVATE ash_combat pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)
create_pbp_file(TARGET ash_cbtest TITLE "Ash Combat" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/test)
