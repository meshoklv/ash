add_executable(ash_game main.c anim.c chr.c camera.c input.c scene.c pcol.c)
target_link_libraries(ash_game PRIVATE pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)

create_pbp_file(
  TARGET ash_game
  TITLE "Ash M4"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

# "Ash: Asylum" - the whole first location (docs/asylum.md): zone + events + combat + hollows (Lua AI) + the demon
# + progression + HUD. anim.c / chr.c come with ash_boss.
add_executable(ash_asylum asy_main.c asy_game.c asy_auto.c camera.c input.c scene.c pcol.c ../boss/boss_ai.c)
target_include_directories(ash_asylum PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_asylum PRIVATE ash_world ash_event ash_combat ash_boss ash_ai ash_progress ash_ui ash_items
                      pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug psppower m)
if(TARGET ash_render)
  target_link_libraries(ash_asylum PRIVATE ash_render)
  target_compile_definitions(ash_asylum PRIVATE ASY_RENDER=1)
endif()
# PSP-est in the game itself (docs/psp-cost-model.md, the perf-night design notes): cmake -DASH_PERF=ON -> cost model, PSPEST log,
# CPU split by system (perf_begin/perf_end in asy_main.c). Off = the macros are empty, nothing changes.
option(ASH_PERF "link src/perf (PSP-3000 cost model) into ash_asylum" OFF)
if(ASH_PERF)
  find_program(ASH_PY python3 REQUIRED)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
    COMMAND ${ASH_PY} ${CMAKE_SOURCE_DIR}/tools/perf/cfg2h.py ${CMAKE_SOURCE_DIR}/src/perf/psp_cost.cfg ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
    DEPENDS ${CMAKE_SOURCE_DIR}/src/perf/psp_cost.cfg)
  target_sources(ash_asylum PRIVATE ../perf/perf.c ../perf/ge_scan.c ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h)
  target_include_directories(ash_asylum PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ../perf)
  target_compile_definitions(ash_asylum PRIVATE ASH_PERF=1)
  target_link_options(ash_asylum PRIVATE "-Wl,--wrap=sceGuStart,--wrap=sceGuFinish,--wrap=sceGuSync,--wrap=sceGuSwapBuffers,--wrap=sceGuDrawBuffer")
endif()
create_pbp_file(
  TARGET ash_asylum
  TITLE "Ash: Asylum"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/asylum
)
