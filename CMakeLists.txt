cmake_minimum_required(VERSION 3.16)
project(ash C CXX)

if(NOT PSP)
  message(FATAL_ERROR "Configure with psp-cmake (pspdev toolchain): . scripts/env.sh && psp-cmake -B build")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-O2 -G0 -Wall)

add_subdirectory(src/diag)   # hardware diagnostics: log.txt, crash.txt, hang watchdog (src/diag/diag.h)
add_subdirectory(src/hello)
add_subdirectory(src/viewer)
add_subdirectory(src/bench)
add_subdirectory(src/ai)   # ash_lua50 + ash_ai (+ ash_aitest EBOOT); skipped with a warning without ~/opt/lua50
add_subdirectory(src/combat)
add_subdirectory(src/event)   # map event interpreter (.ashevt): ash_event + ash_evtest EBOOT
add_subdirectory(src/render)  # optional look layer (docs/render.md): ash_render, linked by ash_asylum if present
add_subdirectory(src/game)
add_subdirectory(src/world)  # zone world: ash_world lib + ash_world_test EBOOT (docs/world.md)
add_subdirectory(src/boss)   # Asylum Demon runtime + ash_bosstest EBOOT
add_subdirectory(src/ui)        # HUD, menus, text (DejaVu + DSR FMG/banners): ash_ui
add_subdirectory(src/menu)      # DS1 menus: title, character creation, pause/inventory/equipment/status/bonfire (ash_menu)
add_subdirectory(src/items)     # items, inventory, equipment, stats, loot (ash_items + ash_items_menu)
add_subdirectory(src/progress)  # Estus, souls, bloodstain, death/respawn, bonfire rest, save: ash_progress
add_subdirectory(src/save)      # save game: format + sceUtilitySavedata A/B + file (docs/save.md), ash_savetest EBOOT
add_subdirectory(src/hudtest)   # ash_hudtest EBOOT: HUD + progression over the Asylum hall
add_subdirectory(src/game/ds)   # "Ash: Dark Souls" - the full game: title -> class -> Asylum -> Firelink (ash_ds)
