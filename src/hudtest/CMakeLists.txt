# Test EBOOT for src/progress + src/ui over the Asylum hall (scripts/shoot-hud.sh). Reuses the game's renderers.
set(GAME ${CMAKE_SOURCE_DIR}/src/game)
add_executable(ash_hudtest main.c ${GAME}/scene.c ${GAME}/chr.c ${GAME}/anim.c)
target_include_directories(ash_hudtest PRIVATE ${GAME})
target_link_libraries(ash_hudtest PRIVATE ash_progress ash_ui pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)

create_pbp_file(
  TARGET ash_hudtest
  TITLE "Ash HUD"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
