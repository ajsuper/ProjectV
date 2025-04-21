cmake_minimum_required(VERSION 3.10)
project(TerrainGenerator)

# Set the directory where projectV is located
set(PROJECTV_DIRECTORY ../../../)

# Include and library directories
include_directories(${PROJECTV_DIRECTORY}/include)
link_directories(${PROJECTV_DIRECTORY}/lib)

# Define the executable
add_executable(TerrainGenerator main.cpp)

# Link against the required libraries
target_link_libraries(TerrainGenerator
    projectV-ecs
    projectV-math
    projectV-fbo
    projectV-render
    projectV-shader
    projectV-uniforms
    projectV-user_input
    projectV-window
    projectV-scene
    projectV-lod
    projectV-voxel_io
    projectV-voxel_management
    projectV-voxel_math
    GLEW
    glfw
    GL
)

# Set compile options
target_compile_options(TerrainGenerator PRIVATE -O3 -fopenmp)
