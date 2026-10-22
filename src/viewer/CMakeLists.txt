add_executable(ash_viewer main.c)
target_link_libraries(ash_viewer PRIVATE pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)

create_pbp_file(
  TARGET ash_viewer
  TITLE "Ash Viewer"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
