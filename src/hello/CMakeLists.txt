add_executable(ash_hello main.c)
target_link_libraries(ash_hello PRIVATE pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug)

create_pbp_file(
  TARGET ash_hello
  TITLE "Ash M0 Hello"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
