# Progression: Estus, souls, bloodstain, death / respawn, bonfire rest, save (docs/progress-ui.md).
# progress.c + save.c are plain C99 (native test: scripts/test-progress.sh); prg_hud.c needs ash_ui.
add_library(ash_progress STATIC progress.c save.c prg_hud.c)
target_include_directories(ash_progress PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_progress PUBLIC ash_ui)
