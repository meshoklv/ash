# DS1-style menus: title, main menu, character creation, in-game menus (docs/menu.md). Library for the game.
# Needs src/ui (ash_ui) and, from the game executable, src/game/chr.c anim.c scene.c (ash_load_file).
add_library(ash_menu STATIC menu.c menu_create.c menu_game.c)
target_include_directories(ash_menu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/ui ${CMAKE_SOURCE_DIR}/src/game)
if(TARGET ash_ui)
  target_link_libraries(ash_menu PUBLIC ash_ui)
endif()
target_link_libraries(ash_menu PUBLIC pspgum pspgu pspctrl m)
