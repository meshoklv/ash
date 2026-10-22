# src/render (docs/render.md): library ash_render + test EBOOT ash_rtest (before/after shots).
# Works both as a subdirectory of the root CMakeLists (add_subdirectory(src/render)) and standalone:
#   . scripts/env.sh && psp-cmake -S src/render -B build/render -G Ninja -DCMAKE_BUILD_TYPE=Release
#   nice -n 19 cmake --build build/render     -> build/render/pbp_rtest/EBOOT.PBP (+ pbp_rtest_est with PSP-est)
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ash_render C)
  if(NOT PSP)
    message(FATAL_ERROR "Configure with psp-cmake (pspdev toolchain)")
  endif()
  set(CMAKE_C_STANDARD 99)
  add_compile_options(-O2 -G0 -Wall)
endif()

set(ASH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(ASH_GAME ${ASH_ROOT}/src/game)

add_library(ash_render STATIC render.c fx.c render_chr.c render_world.c render_chrf.c)
target_include_directories(ash_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${ASH_GAME} ${ASH_ROOT}/src/world)

set(RTEST_SRC test/main.c ${ASH_GAME}/scene.c ${ASH_GAME}/chr.c ${ASH_GAME}/anim.c ${ASH_GAME}/pcol.c)
set(RTEST_LIBS pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)
add_executable(ash_rtest ${RTEST_SRC})
target_link_libraries(ash_rtest PRIVATE ash_render ${RTEST_LIBS})
create_pbp_file(TARGET ash_rtest TITLE "Ash Render Test" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pbp_rtest)

# Optional PSP-est variant (src/perf of agent B, zero-touch --wrap of five sceGu calls): PSPEST line per frame.
if(EXISTS ${ASH_ROOT}/src/perf/perf.c AND EXISTS ${ASH_ROOT}/tools/perf/cfg2h.py)
  find_program(PY python3)
  if(PY)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
      COMMAND ${PY} ${ASH_ROOT}/tools/perf/cfg2h.py ${ASH_ROOT}/src/perf/psp_cost.cfg ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
      DEPENDS ${ASH_ROOT}/src/perf/psp_cost.cfg ${ASH_ROOT}/tools/perf/cfg2h.py)
    add_library(ash_perf_rt STATIC ${ASH_ROOT}/src/perf/perf.c ${ASH_ROOT}/src/perf/ge_scan.c
                ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h)
    target_include_directories(ash_perf_rt PUBLIC ${ASH_ROOT}/src/perf ${CMAKE_CURRENT_BINARY_DIR})
    add_executable(ash_rtest_est ${RTEST_SRC})
    target_link_libraries(ash_rtest_est PRIVATE ash_perf_rt ash_render ${RTEST_LIBS} psppower)
    target_link_options(ash_rtest_est PRIVATE
      "-Wl,--wrap=sceGuStart,--wrap=sceGuFinish,--wrap=sceGuSync,--wrap=sceGuSwapBuffers,--wrap=sceGuDrawBuffer")
    create_pbp_file(TARGET ash_rtest_est TITLE "Ash Render Test PSP-est" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pbp_rtest_est)
    # zone draw A/B bench (the perf-night design notes): world_draw vs render_world on fixed cameras, PSPEST per frame
    add_executable(ash_wbench_est test/wbench.c ${ASH_ROOT}/src/world/world.c ${ASH_GAME}/pcol.c)
    target_link_libraries(ash_wbench_est PRIVATE ash_perf_rt ash_render ${RTEST_LIBS} psppower)
    target_link_options(ash_wbench_est PRIVATE
      "-Wl,--wrap=sceGuStart,--wrap=sceGuFinish,--wrap=sceGuSync,--wrap=sceGuSwapBuffers,--wrap=sceGuDrawBuffer")
    create_pbp_file(TARGET ash_wbench_est TITLE "Ash wbench PSP-est" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pbp_wbench_est)
  endif()
endif()
