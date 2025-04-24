cmake_minimum_required(VERSION 3.10)
project(ProjectV)

# Compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O3 -fopenmp")

message(STATUS "CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")

include_directories(${CMAKE_SOURCE_DIR}/include)

# Add spdlog submodule (this assumes you've added it correctly as a submodule)
add_subdirectory(external/spdlog)

include_directories(${CMAKE_SOURCE_DIR}/external/spdlog/include)

# Source files
set(CORE_SRC_DIR ${CMAKE_SOURCE_DIR}/src/core)
set(GRAPHICS_SRC_DIR ${CMAKE_SOURCE_DIR}/src/graphics)
set(UTILS_SRC_DIR ${CMAKE_SOURCE_DIR}/src/utils)

# Output directories
set(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIBRARY_OUTPUT_PATH})

# Core
add_library(projectV-ecs STATIC ${CORE_SRC_DIR}/ecs.cpp)
add_library(projectV-math STATIC ${CORE_SRC_DIR}/math.cpp)
add_library(projectV-log STATIC ${CORE_SRC_DIR}/log.cpp)

# Graphics
add_library(projectV-fbo STATIC ${GRAPHICS_SRC_DIR}/fbo.cpp)
add_library(projectV-render STATIC ${GRAPHICS_SRC_DIR}/render.cpp)
add_library(projectV-scene STATIC ${GRAPHICS_SRC_DIR}/scene.cpp)
find_package(OpenMP REQUIRED)
target_link_libraries(projectV-scene PRIVATE OpenMP::OpenMP_CXX)

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

# Link spdlog to all libraries
target_include_directories(projectV-ecs PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-math PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)

target_include_directories(projectV-fbo PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-render PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-scene PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-shader PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-uniforms PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-user_input PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-window PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)

target_include_directories(projectV-lod PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-voxel_io PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-voxel_management PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
target_include_directories(projectV-voxel_math PRIVATE ${CMAKE_SOURCE_DIR}/external/spdlog/include)


