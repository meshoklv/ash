# "Ash: Dark Souls" - the full game EBOOT (src/game/ds/ds_main.c, docs/game.md): title -> character creation ->
# Northern Undead Asylum -> crow -> Firelink Shrine, menus, inventory, save. Simulation = the Asylum game files of
# src/game (asy_game.c, asy_auto.c) + every module. Needs src/render (fast zone/character paths).
set(GAME ${CMAKE_SOURCE_DIR}/src/game)
add_executable(ash_ds ds_main.c ${GAME}/asy_game.c ${GAME}/asy_auto.c ${GAME}/camera.c ${GAME}/input.c ${GAME}/scene.c
               ${GAME}/pcol.c ${CMAKE_SOURCE_DIR}/src/boss/boss_ai.c)
target_include_directories(ash_ds PRIVATE ${GAME})
target_link_libraries(ash_ds PRIVATE ash_menu ash_items_menu ash_items ash_save ash_render ash_world ash_event ash_combat
                      ash_boss ash_ai ash_progress ash_ui pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug psppower
                      psputility m)
target_compile_definitions(ash_ds PRIVATE ASY_RENDER=1)
ash_diag_link(ash_ds)   # src/diag: log.txt, crash.txt, hang watchdog, ash_ds.map + unstripped ash_ds.elf
add_dependencies(ash_ds ashexc)
# PSP-est in the game (docs/psp-cost-model.md): cmake -DASH_PERF=ON (option declared in src/game/CMakeLists.txt)
if(ASH_PERF)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
    COMMAND ${ASH_PY} ${CMAKE_SOURCE_DIR}/tools/perf/cfg2h.py ${CMAKE_SOURCE_DIR}/src/perf/psp_cost.cfg ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
    DEPENDS ${CMAKE_SOURCE_DIR}/src/perf/psp_cost.cfg)
  target_sources(ash_ds PRIVATE ${CMAKE_SOURCE_DIR}/src/perf/perf.c ${CMAKE_SOURCE_DIR}/src/perf/ge_scan.c
                 ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h)
  target_include_directories(ash_ds PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src/perf)
  target_compile_definitions(ash_ds PRIVATE ASH_PERF=1)
  target_link_options(ash_ds PRIVATE "-Wl,--wrap=sceGuStart,--wrap=sceGuFinish,--wrap=sceGuSync,--wrap=sceGuSwapBuffers,--wrap=sceGuDrawBuffer")
endif()
create_pbp_file(
  TARGET ash_ds
  TITLE "Ash: Dark Souls"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
