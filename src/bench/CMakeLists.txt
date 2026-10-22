# Ash M2 hardware bench (PLAN.md 6.6 / 7.1). Two EBOOTs:
#   ash_bench  - full bench, PARAM.SFO MEMSIZE=0 (32 MB profile, the main one)  -> build/src/bench/ASHBENCH/EBOOT.PBP
#   ash_mem64  - memory/GATE3 only, MEMSIZE=1 (64 MB mode on PSP-2000/3000)    -> build/src/bench/ASHMEM64/EBOOT.PBP
if(EXISTS ${CMAKE_SOURCE_DIR}/.ash_sha)   # snapshot made by scripts/make-dist.sh
  file(STRINGS ${CMAKE_SOURCE_DIR}/.ash_sha ASH_GIT_SHA LIMIT_COUNT 1)
else()
  execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE ASH_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT ASH_GIT_SHA)
  set(ASH_GIT_SHA "nogit")
endif()

set(BENCH_LIBS pspgum_vfpu pspvfpu pspgu pspge pspdisplay pspctrl psprtc psppower pspdebug pspkubridge m)

add_executable(ash_bench main.c)
target_compile_definitions(ash_bench PRIVATE ASH_GIT_SHA="${ASH_GIT_SHA}")
target_link_libraries(ash_bench PRIVATE ${BENCH_LIBS})
ash_diag_link(ash_bench)   # src/diag: log.txt, crash.txt, hang watchdog (stage ashexc.prx next to the EBOOT)
create_pbp_file(TARGET ash_bench TITLE "Ash Bench" MEMSIZE 0
  ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/icon_bench.png
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/ASHBENCH)

add_executable(ash_mem64 main.c)
target_compile_definitions(ash_mem64 PRIVATE ASH_GIT_SHA="${ASH_GIT_SHA}" ASH_MEM_ONLY=1)
target_compile_options(ash_mem64 PRIVATE -Wno-unused-function -Wno-unused-variable)
target_link_libraries(ash_mem64 PRIVATE ${BENCH_LIBS})
ash_diag_link(ash_mem64)
create_pbp_file(TARGET ash_mem64 TITLE "Ash Bench: memory 64MB" MEMSIZE 1
  ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/icon_mem64.png
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/ASHMEM64)
