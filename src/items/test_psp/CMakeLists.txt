# Stand-alone PSP test EBOOT for src/items (+ src/menu with src/items/menu-ext-images.patch applied to a copy in the
# build dir - src/menu itself is not touched). Own build dir:
#   . scripts/env.sh && psp-cmake -S src/items/test_psp -B build/items-psp -G Ninja -DCMAKE_BUILD_TYPE=Release
#   nice -n 19 cmake --build build/items-psp        # -> build/items-psp/EBOOT.PBP; run: src/items/test_psp/shoot.sh
cmake_minimum_required(VERSION 3.16)
project(ash_itemstest C)
if(NOT PSP)
  message(FATAL_ERROR "Configure with psp-cmake (pspdev toolchain)")
endif()
set(CMAKE_C_STANDARD 99)
add_compile_options(-O2 -G0 -Wall)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(REPO ${SRC}/.. ABSOLUTE)
set(MP ${CMAKE_CURRENT_BINARY_DIR}/mp)
# patched copy of src/menu (re-made on every configure)
file(REMOVE_RECURSE ${MP})
file(GLOB MENU_FILES ${SRC}/menu/*.c ${SRC}/menu/*.h)
file(COPY ${MENU_FILES} DESTINATION ${MP}/src/menu)
file(RELATIVE_PATH MP_REL ${REPO} ${MP})
execute_process(COMMAND git apply --unsafe-paths --directory=${MP_REL} ${SRC}/items/menu-ext-images.patch
                WORKING_DIRECTORY ${REPO} RESULT_VARIABLE PATCH_RC)
if(NOT PATCH_RC EQUAL 0)
  message(FATAL_ERROR "src/items/menu-ext-images.patch does not apply to src/menu any more")
endif()
add_executable(ash_itemstest main.c ${SRC}/items/items.c ${SRC}/items/items_menu.c
  ${MP}/src/menu/menu.c ${MP}/src/menu/menu_create.c ${MP}/src/menu/menu_game.c
  ${SRC}/ui/ui.c ${SRC}/game/chr.c ${SRC}/game/anim.c ${SRC}/game/scene.c)
target_include_directories(ash_itemstest PRIVATE ${MP}/src/menu ${SRC}/items ${SRC}/ui ${SRC}/game)
target_link_libraries(ash_itemstest PRIVATE pspgum pspgu pspge pspdisplay pspctrl psprtc pspdebug m)
create_pbp_file(
  TARGET ash_itemstest
  TITLE "Ash Items"
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
