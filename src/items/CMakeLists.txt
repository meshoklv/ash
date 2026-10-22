# src/items: item database, inventory, equipment, stats, use, loot, save (docs/items.md). Plain C99.
# ash_items has no PSP dependency; ash_items_menu (the MenuGameView bridge) needs ash_menu.
add_library(ash_items STATIC items.c)
target_include_directories(ash_items PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_items PUBLIC m)
if(TARGET ash_menu)
  add_library(ash_items_menu STATIC items_menu.c)
  target_link_libraries(ash_items_menu PUBLIC ash_items ash_menu)
endif()
