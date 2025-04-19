cmake_minimum_required(VERSION 3.10)
project(ProjectV)

# Compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O3")

include_directories(${CMAKE_SOURCE_DIR}/include)

# Old ->
# Source files
set(CORE_SRC_DIR ${CMAKE_SOURCE_DIR}/src/core)
set(GRAPHICS_SRC_DIR ${CMAKE_SOURCE_DIR}/src/graphics)
set(UTILS_SRC_DIR ${CMAKE_SOURCE_DIR}/src/utils)

# Output directories
set(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIBRARY_OUTPUT_PATH})

# Core
add_library(projectV-ecs STATIC ${CORE_SRC_DIR}/ecs.cpp)

# Graphics
add_library(projectV-fbo STATIC ${GRAPHICS_SRC_DIR}/fbo.cpp)
add_library(projectV-render STATIC ${GRAPHICS_SRC_DIR}/render.cpp)
add_library(projectV-scene STATIC ${GRAPHICS_SRC_DIR}/scene.cpp)
add_library(projectV-shader STATIC ${GRAPHICS_SRC_DIR}/shader.cpp)
add_library(projectV-uniforms STATIC ${GRAPHICS_SRC_DIR}/uniforms.cpp)
add_library(projectV-user_input STATIC ${GRAPHICS_SRC_DIR}/user_input.cpp)
add_library(projectV-window STATIC ${GRAPHICS_SRC_DIR}/window.cpp)

# Utils
add_library(projectV-lod STATIC ${UTILS_SRC_DIR}/lod.cpp)
target_link_libraries(projectV-lod PRIVATE projectV-voxel_io)
add_library(projectV-voxel_io STATIC ${UTILS_SRC_DIR}/voxel_io.cpp)
add_library(projectV-voxel_management STATIC ${UTILS_SRC_DIR}/voxel_management.cpp)
add_library(projectV-voxel_math STATIC ${UTILS_SRC_DIR}/voxel_math.cpp)

