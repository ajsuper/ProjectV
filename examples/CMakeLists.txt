# Bundled examples, in reading order. Each subdirectory declares itself with projv_add_example();
# the helper lives in cmake/ProjectVExample.cmake alongside the shader rule it uses.
#
# The numbering is the order they are meant to be read, not a priority: 00 is the smallest thing
# that draws a voxel, 60 is a complete editor.
include(ProjectVExample)

add_subdirectory(00-hello-voxel)
add_subdirectory(10-scene-previewer)
add_subdirectory(30-renderers)
add_subdirectory(40-advanced-renderer)
add_subdirectory(50-terrain-generator)

# Examples with third-party dependencies of their own. Each is skipped, with a reason, when its
# submodule is not checked out -- an uninitialised submodule should not fail the whole build.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/20-mesh-voxelizer/external/assimp/CMakeLists.txt")
    add_subdirectory(20-mesh-voxelizer)
else()
    message(STATUS "examples: skipping 20-mesh-voxelizer (needs examples/20-mesh-voxelizer/external/assimp; "
                   "run `git submodule update --init --recursive`)")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/60-scene-editor/external/lua/lua.c"
   AND EXISTS "${CMAKE_SOURCE_DIR}/external/imgui/imgui.cpp")
    add_subdirectory(60-scene-editor)
else()
    message(STATUS "examples: skipping 60-scene-editor (needs external/imgui and "
                   "examples/60-scene-editor/external/lua; run `git submodule update --init --recursive`)")
endif()
