# Ash PSP-est builds (docs/psp-cost-model.md). A standalone project so nothing shared has to change:
#   . scripts/env.sh && psp-cmake -S src/perf -B build/perf -G Ninja -DCMAKE_BUILD_TYPE=Release && nice -n 19 cmake --build build/perf
# MEMSIZE as in the original targets (game/viewer: create_pbp_file default 2, bench: 0).
# Targets: the unchanged game / viewer / bench sources + src/perf, linked with --wrap on five sceGu calls
#   build/perf/pbp_ash_{game,viewer,bench}_est/EBOOT.PBP
cmake_minimum_required(VERSION 3.16)
project(ash_perf C)
if(NOT PSP)
  message(FATAL_ERROR "Configure with psp-cmake (pspdev toolchain)")
endif()
set(CMAKE_C_STANDARD 99)
add_compile_options(-O2 -G0 -Wall)

set(ASH ${CMAKE_CURRENT_SOURCE_DIR}/../..)
find_program(PY python3 REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
  COMMAND ${PY} ${ASH}/tools/perf/cfg2h.py ${CMAKE_CURRENT_SOURCE_DIR}/psp_cost.cfg ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/psp_cost.cfg ${ASH}/tools/perf/cfg2h.py)
add_library(ash_perf STATIC perf.c ge_scan.c ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h)
target_include_directories(ash_perf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

set(PERF_WRAP "-Wl,--wrap=sceGuStart,--wrap=sceGuFinish,--wrap=sceGuSync,--wrap=sceGuSwapBuffers,--wrap=sceGuDrawBuffer")

function(ash_est name title srcs libs memsize)
  add_executable(${name} ${srcs})
  # perf first: its __wrap_* must be seen; pspgu after so __real_* resolve
  target_link_libraries(${name} PRIVATE ash_perf ${libs} psppower pspdebug m)
  target_link_options(${name} PRIVATE ${PERF_WRAP})
  create_pbp_file(TARGET ${name} TITLE "${title}" MEMSIZE ${memsize} OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pbp_${name})
endfunction()

set(G ${ASH}/src/game)
set(GAME_SRC ${G}/main.c ${G}/anim.c ${G}/chr.c ${G}/camera.c ${G}/input.c ${G}/scene.c ${G}/pcol.c)
ash_est(ash_game_est "Ash M4 PSP-est" "${GAME_SRC}" "pspgum;pspgu;pspge;pspdisplay;pspctrl;psprtc" 2)
ash_est(ash_viewer_est "Ash Viewer PSP-est" "${ASH}/src/viewer/main.c" "pspgum;pspgu;pspge;pspdisplay;pspctrl;psprtc" 2)

execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${ASH} OUTPUT_VARIABLE SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
ash_est(ash_bench_est "Ash Bench PSP-est" "${ASH}/src/bench/main.c" "pspgum_vfpu;pspvfpu;pspgu;pspge;pspdisplay;pspctrl;psprtc;pspkubridge" 0)
target_compile_definitions(ash_bench_est PRIVATE ASH_GIT_SHA="${SHA}-est")

# the same bench without src/perf: emulator baseline for calibrate.py (CPU section, free memory)
add_executable(ash_bench_plain ${ASH}/src/bench/main.c)
target_link_libraries(ash_bench_plain PRIVATE pspgum_vfpu pspvfpu pspgu pspge pspdisplay pspctrl psprtc pspkubridge psppower pspdebug m)
target_compile_definitions(ash_bench_plain PRIVATE ASH_GIT_SHA="${SHA}-plain")
create_pbp_file(TARGET ash_bench_plain TITLE "Ash Bench" MEMSIZE 0 OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pbp_ash_bench_plain)

# "Ash: Asylum" (src/game/asy_*.c, the whole first location) with the model: the module libraries are built here from
# their own CMakeLists (unchanged), the game sources are the same as ash_asylum's. ASY_RENDER like the main build.
#   build/perf/pbp_ash_asylum_est/EBOOT.PBP   (scripts/psp-est.sh asylum ...)
set(SUB ${CMAKE_CURRENT_BINARY_DIR}/sub)
add_subdirectory(${ASH}/src/ai ${SUB}/ai)
add_subdirectory(${ASH}/src/combat ${SUB}/combat)
add_subdirectory(${ASH}/src/event ${SUB}/event)
add_subdirectory(${ASH}/src/render ${SUB}/render)
add_subdirectory(${ASH}/src/world ${SUB}/world)
add_subdirectory(${ASH}/src/boss ${SUB}/boss)
add_subdirectory(${ASH}/src/ui ${SUB}/ui)
add_subdirectory(${ASH}/src/progress ${SUB}/progress)
if(TARGET ash_ai)
  set(ASY_SRC ${G}/asy_main.c ${G}/asy_game.c ${G}/asy_auto.c ${G}/camera.c ${G}/input.c ${G}/scene.c ${G}/pcol.c
      ${ASH}/src/boss/boss_ai.c)
  ash_est(ash_asylum_est "Ash Asylum PSP-est" "${ASY_SRC}"
          "ash_render;ash_world;ash_event;ash_combat;ash_boss;ash_ai;ash_progress;ash_ui;pspgum;pspgu;pspge;pspdisplay;pspctrl;psprtc" 2)
  target_include_directories(ash_asylum_est PRIVATE ${G})
  target_compile_definitions(ash_asylum_est PRIVATE ASY_RENDER=1)
  # the same with the speed patch (src/perf/asy_fast.patch, the perf-night design notes) applied to a copy of asy_main.c:
  # fast path by default, demo.txt 4th word "slow" = the old path -> A/B in one EBOOT
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fast/asy_main.c
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/fast
    COMMAND ${PY} ${ASH}/tools/perf/apply_patch.py ${G}/asy_main.c ${CMAKE_CURRENT_SOURCE_DIR}/asy_fast.patch ${CMAKE_CURRENT_BINARY_DIR}/fast/asy_main.c
    DEPENDS ${G}/asy_main.c ${CMAKE_CURRENT_SOURCE_DIR}/asy_fast.patch ${ASH}/tools/perf/apply_patch.py)
  set(ASYF_SRC ${CMAKE_CURRENT_BINARY_DIR}/fast/asy_main.c ${G}/asy_game.c ${G}/asy_auto.c ${G}/camera.c ${G}/input.c
      ${G}/scene.c ${G}/pcol.c ${ASH}/src/boss/boss_ai.c)
  ash_est(ash_asylum_fast_est "Ash Asylum fast PSP-est" "${ASYF_SRC}"
          "ash_render;ash_world;ash_event;ash_combat;ash_boss;ash_ai;ash_progress;ash_ui;pspgum;pspgu;pspge;pspdisplay;pspctrl;psprtc" 2)
  target_include_directories(ash_asylum_fast_est PRIVATE ${G})
  target_compile_definitions(ash_asylum_fast_est PRIVATE ASY_RENDER=1 ASH_PERF=1)
endif()
