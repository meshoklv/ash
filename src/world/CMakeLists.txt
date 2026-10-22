# World of a zone (src/world/world.h): library for the game + a test EBOOT that renders camera tours of the pak.
add_library(ash_world STATIC world.c)
target_include_directories(ash_world PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_world PUBLIC pspgum pspgu pspge m)

add_executable(ash_world_test test.c ../game/pcol.c)   # pcol: same collision code as the game
target_link_libraries(ash_world_test PRIVATE ash_world pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)

create_pbp_file(
  TARGET ash_world_test
  TITLE "Ash World"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
