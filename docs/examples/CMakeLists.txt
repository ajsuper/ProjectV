# Bundled examples. Each subdirectory declares itself with projv_add_example(); the helper
# lives in cmake/ProjectVExample.cmake alongside the shader rule it uses.
include(ProjectVExample)

add_subdirectory(editing_p1)
add_subdirectory(ScenePreviewer)
add_subdirectory(PathTracer)
add_subdirectory(AdvancedRenderer)
add_subdirectory(terrain_generator)
add_subdirectory(edit_demo)

# Examples with their own third-party dependencies. Each is skipped, with a reason, when its
# submodule is not checked out -- an uninitialised submodule should not fail the whole build.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/MeshVoxelizer/external/assimp/CMakeLists.txt")
    add_subdirectory(MeshVoxelizer)
else()
    message(STATUS "examples: skipping MeshVoxelizer (needs docs/examples/MeshVoxelizer/external/assimp; "
                   "run `git submodule update --init --recursive`)")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SceneEditor/external/lua/lua.c"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../external/imgui/imgui.cpp")
    add_subdirectory(SceneEditor)
else()
    message(STATUS "examples: skipping SceneEditor (needs external/imgui and "
                   "docs/examples/SceneEditor/external/lua; run `git submodule update --init --recursive`)")
endif()
