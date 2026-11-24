# 56-santa-fe: the Santa Fe mountains as a quality reference -- a slow, progressive renderer and richer
# generation, to find out what a future optimised version needs. Forked from 55-mountains.
# Plan: docs/plans/santa-fe-reference.md.

add_executable(santa_fe_lab tools/lab.cpp)
target_include_directories(santa_fe_lab PRIVATE include)
target_link_libraries(santa_fe_lab PRIVATE bgfx::bimg bgfx::bx)
set_target_properties(santa_fe_lab PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/santa_fe")

projv_add_example(santa_fe
    SOURCES             main.cpp
    INCLUDE_DIRS        include
    SHADERS             renderer/shaders
    SHADER_INCLUDE_DIRS renderer/include
    ASSET_DIRS          renderer
)

# Trees grown by the scene editor's tree brush (examples/60-scene-editor/brushes/trees.lua), through its
# own brush host (brush.cpp) and Lua -- as 50-terrain-generator's terrain_asset_baker does -- at startup,
# cached beside the binary. Without the editor's Lua submodule, the procedural trees (trees.hpp) stand in.
set(SANTA_FE_EDITOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../60-scene-editor")
if(EXISTS "${SANTA_FE_EDITOR_DIR}/external/lua/lua.h")
    file(GLOB SANTA_FE_LUA_SOURCES "${SANTA_FE_EDITOR_DIR}/external/lua/*.c")
    list(FILTER SANTA_FE_LUA_SOURCES EXCLUDE REGEX "/(lua|onelua|ltests)\\.c$")
    add_library(santa_fe_lua STATIC ${SANTA_FE_LUA_SOURCES})
    target_include_directories(santa_fe_lua PUBLIC "${SANTA_FE_EDITOR_DIR}/external/lua")
    if(NOT WIN32)
        target_compile_definitions(santa_fe_lua PRIVATE LUA_USE_POSIX)
    endif()
    target_compile_options(santa_fe_lua PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
    target_sources(santa_fe PRIVATE "${SANTA_FE_EDITOR_DIR}/brush.cpp")
    target_include_directories(santa_fe PRIVATE "${SANTA_FE_EDITOR_DIR}")
    target_link_libraries(santa_fe PRIVATE santa_fe_lua)
    target_compile_definitions(santa_fe PRIVATE SANTA_FE_BRUSH_TREES=1
        SANTA_FE_BRUSHES_DIR="${SANTA_FE_EDITOR_DIR}/brushes")
else()
    message(STATUS "santa_fe: no Lua at ${SANTA_FE_EDITOR_DIR}/external/lua; procedural trees instead of the tree brush's")
endif()
