# Save game (docs/save.md): format (savegame.c, plain C99 - native test scripts/test-save.sh) and storage
# (savestore.c: sceUtilitySavedata A/B + file). ash_savetest EBOOT: scripts/test-save-emu.sh.
add_library(ash_save STATIC savegame.c savestore.c)
target_include_directories(ash_save PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_save PUBLIC ash_progress psputility psprtc)

add_executable(ash_savetest test/test.c)
target_include_directories(ash_savetest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
target_link_libraries(ash_savetest PRIVATE ash_save pspdebug pspdisplay pspge psputility psprtc m)
create_pbp_file(
  TARGET ash_savetest
  TITLE "Ash save test"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
