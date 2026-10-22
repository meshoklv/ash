# src/firelink (docs/firelink.md): test EBOOT of the Firelink Shrine zone (m10_02): camera orbit, view from the
# bonfire, the knight at the bonfire with fire particles, the Crestfallen Warrior seated. Standalone:
#   . scripts/env.sh && psp-cmake -S src/firelink -B build/firelink -G Ninja -DCMAKE_BUILD_TYPE=Release
#   nice -n 19 cmake --build build/firelink -j4   -> build/firelink/pbp_fltest{,_est}/EBOOT.PBP
# Uses src/world (world.c: pak loader), src/render (mips, fog, char light, fx), src/game (chr/anim/pcol/scene) as
# sources, without touching them.
cmake_minimum_required(VERSION 3.16)
project(ash_firelink C)
if(NOT PSP)
  message(FATAL_ERROR "Configure with psp-cmake (pspdev toolchain)")
endif()
set(CMAKE_C_STANDARD 99)
add_compile_options(-O2 -G0 -Wall)

set(ASH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(ASH_GAME ${ASH_ROOT}/src/game)
set(ASH_RENDER ${ASH_ROOT}/src/render)
set(ASH_WORLD ${ASH_ROOT}/src/world)

set(FL_SRC test/main.c
    ${ASH_WORLD}/world.c
    ${ASH_RENDER}/render.c ${ASH_RENDER}/fx.c
    ${ASH_GAME}/scene.c ${ASH_GAME}/chr.c ${ASH_GAME}/anim.c ${ASH_GAME}/pcol.c)
set(FL_LIBS pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)

add_executable(ash_fltest ${FL_SRC})
target_include_directories(ash_fltest PRIVATE ${ASH_GAME} ${ASH_RENDER} ${ASH_WORLD})
target_link_libraries(ash_fltest PRIVATE ${FL_LIBS})
create_pbp_file(TARGET ash_fltest TITLE "Ash Firelink" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pbp_fltest)

# PSP-est variant (src/perf, zero-touch --wrap of five sceGu calls): a PSPEST line per frame
if(EXISTS ${ASH_ROOT}/src/perf/perf.c AND EXISTS ${ASH_ROOT}/tools/perf/cfg2h.py)
  find_program(PY python3)
  if(PY)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
      COMMAND ${PY} ${ASH_ROOT}/tools/perf/cfg2h.py ${ASH_ROOT}/src/perf/psp_cost.cfg ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h
      DEPENDS ${ASH_ROOT}/src/perf/psp_cost.cfg ${ASH_ROOT}/tools/perf/cfg2h.py)
    add_library(fl_perf_rt STATIC ${ASH_ROOT}/src/perf/perf.c ${ASH_ROOT}/src/perf/ge_scan.c
                ${CMAKE_CURRENT_BINARY_DIR}/perf_cost_gen.h)
    target_include_directories(fl_perf_rt PUBLIC ${ASH_ROOT}/src/perf ${CMAKE_CURRENT_BINARY_DIR})
    add_executable(ash_fltest_est ${FL_SRC})
    target_include_directories(ash_fltest_est PRIVATE ${ASH_GAME} ${ASH_RENDER} ${ASH_WORLD})
    target_link_libraries(ash_fltest_est PRIVATE fl_perf_rt ${FL_LIBS} psppower)
    target_link_options(ash_fltest_est PRIVATE
      "-Wl,--wrap=sceGuStart,--wrap=sceGuFinish,--wrap=sceGuSync,--wrap=sceGuSwapBuffers,--wrap=sceGuDrawBuffer")
    create_pbp_file(TARGET ash_fltest_est TITLE "Ash Firelink PSP-est" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pbp_fltest_est)
  endif()
endif()
