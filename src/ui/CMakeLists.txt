# HUD / menus / text (sceGu): docs/progress-ui.md
add_library(ash_ui STATIC ui.c hud.c)
target_include_directories(ash_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_ui PUBLIC pspgu pspgum m)
