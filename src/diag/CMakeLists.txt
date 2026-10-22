# src/diag: black box log + hang watchdog + crash catcher for the real PSP (diag.h, docs/hw-pitfalls.md).
#   ash_diag  - static lib for the game / bench. Link with  ash_diag_link(<target>)  (adds the malloc wraps).
#   ashexc    - tiny KERNEL module (exc/), staged as ashexc.prx next to the EBOOT; the game loads it through
#               KUBridge (PRO / ARK) to register the CPU exception handler.
enable_language(ASM)

if(EXISTS ${CMAKE_SOURCE_DIR}/.ash_sha)   # snapshot made by scripts/make-dist.sh
  file(STRINGS ${CMAKE_SOURCE_DIR}/.ash_sha ASH_DIAG_SHA LIMIT_COUNT 1)
else()
  execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE ASH_DIAG_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT ASH_DIAG_SHA)
  set(ASH_DIAG_SHA "nogit")
endif()
set(ASH_DIAG_SHA ${ASH_DIAG_SHA} PARENT_SCOPE)

add_library(ash_diag STATIC diag.c diag_asm.S)
target_include_directories(ash_diag PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_diag PUBLIC pspkubridge pspge pspdisplay pspctrl psprtc pspdebug)

function(ash_diag_link tgt)
  target_link_libraries(${tgt} PRIVATE ash_diag)
  target_compile_definitions(${tgt} PRIVATE ASH_DIAG_SHA="${ASH_DIAG_SHA}")
  target_link_options(${tgt} PRIVATE "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign"
                      "-Wl,-Map=$<TARGET_FILE:${tgt}>.map")
  # keep an unstripped copy for psp-addr2line (create_pbp_file strips the target in place afterwards)
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${tgt}> $<TARGET_FILE:${tgt}>.elf)
endfunction()

# kernel module: no libc, no crt, kernel stubs only
add_executable(ashexc exc/ashexc.c exc/ashexc_asm.S)
target_compile_options(ashexc PRIVATE -fno-builtin)
target_link_options(ashexc PRIVATE -nostartfiles -nostdlib -Wl,-q,-T${PSPDEV}/psp/sdk/lib/linkfile.prx
                    -Wl,-zmax-page-size=128 ${PSPDEV}/psp/sdk/lib/prxexports.o)
target_link_libraries(ashexc PRIVATE -Wl,--start-group pspkernel pspmodinfo -Wl,--end-group)
add_custom_command(TARGET ashexc POST_BUILD
  COMMAND ${PSPDEV}/bin/psp-fixup-imports $<TARGET_FILE:ashexc>
  COMMAND ${PSPDEV}/bin/psp-prxgen $<TARGET_FILE:ashexc> ${CMAKE_CURRENT_BINARY_DIR}/ashexc.prx
  COMMENT "ashexc.prx (kernel module)")
