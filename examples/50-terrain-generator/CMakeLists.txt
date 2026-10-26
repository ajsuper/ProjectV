projv_add_example(terrain_generator
    SOURCES      main.cpp
    INCLUDE_DIRS include ../shared   # restir_pt.h, voxel_renderer.h
    ASSET_DIRS   biomes
)

# The renderer: the renderer gallery's restir-pt, compiled by the gallery's shader target and staged
# here as renderers/restir-pt/ -- where its resources.json names its shaders (examples/shared/restir_pt.h).
set(restirSource "${CMAKE_CURRENT_SOURCE_DIR}/../30-renderers/renderers/restir-pt")
add_dependencies(terrain_generator renderer_gallery_shaders)
set_property(TARGET terrain_generator APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:renderer_gallery_shaders,PROJV_SHADER_OUTPUTS>)
add_custom_command(TARGET terrain_generator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${restirSource}" "${CMAKE_BINARY_DIR}/examples/terrain_generator/renderers/restir-pt"
    COMMENT "Staging restir-pt for terrain_generator"
    VERBATIM
)

# terrain_asset_baker: regrows a biome's assets (biomes/<biome>/assets/) with the scene editor's
# brushes -- its brush host, brush.cpp, and brushes/trees.lua and rocks.lua -- from that biome's
# designs.json. Not needed to run the terrain -- the assets are committed -- only to change them.
# Needs the scene editor's Lua submodule.
set(TREE_EDITOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../60-scene-editor")
if(EXISTS "${TREE_EDITOR_DIR}/external/lua/lua.h")
    file(GLOB TREE_LUA_SOURCES "${TREE_EDITOR_DIR}/external/lua/*.c")
    # Not part of the library: the standalone interpreter, the amalgamation, the test harness.
    list(FILTER TREE_LUA_SOURCES EXCLUDE REGEX "/(lua|onelua|ltests)\\.c$")
    add_library(terrain_asset_baker_lua STATIC ${TREE_LUA_SOURCES})
    target_include_directories(terrain_asset_baker_lua PUBLIC "${TREE_EDITOR_DIR}/external/lua")
    target_compile_definitions(terrain_asset_baker_lua PRIVATE LUA_USE_POSIX)
    target_compile_options(terrain_asset_baker_lua PRIVATE -w) # Third-party C; not our warnings to fix.

    add_executable(terrain_asset_baker tools/bake_assets.cpp "${TREE_EDITOR_DIR}/brush.cpp")
    target_include_directories(terrain_asset_baker PRIVATE "${TREE_EDITOR_DIR}")
    target_link_libraries(terrain_asset_baker PRIVATE ProjectV::projectV terrain_asset_baker_lua)
    target_compile_definitions(terrain_asset_baker PRIVATE
        ASSET_BRUSHES_DIR="${TREE_EDITOR_DIR}/brushes"
        ASSET_BIOMES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/biomes")
    set_target_properties(terrain_asset_baker PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/terrain_generator")
else()
    message(STATUS "terrain_generator: no Lua at ${TREE_EDITOR_DIR}/external/lua; "
                   "terrain_asset_baker (regrows biomes/*/assets/) is skipped")
endif()
