if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)   # standalone: psp-cmake -S src/ai -B build/ai-psp
  cmake_minimum_required(VERSION 3.16)
  project(ash_ai C)
  set(CMAKE_C_STANDARD 99)
  add_compile_options(-O2 -G0 -Wall)
endif()
# Lua 5.0.3 VM (lua.org sources, fetched by tools/ai/build_lua50.sh into ~/opt/lua50/src, never vendored) +
# the AI host. LUA_NUMBER = float on the PSP: DSR bytecode is repacked by tools/ai/luac_repack.py --number float
# (no integer constant above 2^24 in any of the 538 DSR AI chunks, docs/ai-runtime.md).
set(LUA50_SRC "$ENV{HOME}/opt/lua50/src" CACHE PATH "Lua 5.0.3 src/ directory (tools/ai/build_lua50.sh)")
if(NOT EXISTS "${LUA50_SRC}/lvm.c")
  message(WARNING "src/ai: Lua 5.0.3 sources not found in ${LUA50_SRC} - run tools/ai/build_lua50.sh; AI skipped")
  return()
endif()

# core + the libs the scripts use (base, table, string, math). No io/os/loadlib: nothing in DSR AI calls them.
set(LUA50_CORE lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable
    ltm lundump lvm lzio)
set(LUA50_LIBS lauxlib lbaselib ltablib lstrlib lmathlib)
set(LUA50_FILES)
foreach(f ${LUA50_CORE})
  list(APPEND LUA50_FILES "${LUA50_SRC}/${f}.c")
endforeach()
foreach(f ${LUA50_LIBS})
  list(APPEND LUA50_FILES "${LUA50_SRC}/lib/${f}.c")
endforeach()

add_library(ash_lua50 STATIC ${LUA50_FILES})
target_include_directories(ash_lua50 PUBLIC "${LUA50_SRC}/../include" PRIVATE "${LUA50_SRC}")
target_compile_definitions(ash_lua50 PUBLIC LUA_NUMBER=float "LUA_NUMBER_SCAN=\"%f\"" "LUA_NUMBER_FMT=\"%.7g\"")
target_compile_options(ash_lua50 PRIVATE -w)

add_library(ash_ai STATIC aihost.c)
target_include_directories(ash_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ash_ai PUBLIC ash_lua50 m)

add_executable(ash_aitest aitest.c)
target_link_libraries(ash_aitest PRIVATE ash_ai psppower psprtc)
create_pbp_file(TARGET ash_aitest TITLE "Ash AI test" OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
