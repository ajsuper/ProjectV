cmake_minimum_required(VERSION 3.10)
project(ProjectV)

# Compiler settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O3 -fopenmp")

message(STATUS "CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")

# Output directories
set(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIBRARY_OUTPUT_PATH})

# External dependencies (submodules)
add_subdirectory(external/spdlog)
add_subdirectory(external/json)
add_subdirectory(external/glm)

# Header include paths (external)
set(SPDLOG_INCLUDE ${CMAKE_SOURCE_DIR}/external/spdlog/include)
set(JSON_INCLUDE ${CMAKE_SOURCE_DIR}/external/json/single_include)
set(GLM_INCLUDE ${CMAKE_SOURCE_DIR}/external/glm)

# Header include paths (local)
set(LOCAL_INCLUDE ${CMAKE_SOURCE_DIR}/include)

# Source directories
set(CORE_SRC_DIR ${CMAKE_SOURCE_DIR}/src/core)
set(GRAPHICS_SRC_DIR ${CMAKE_SOURCE_DIR}/src/graphics)
set(UTILS_SRC_DIR ${CMAKE_SOURCE_DIR}/src/utils)

# Macro for linking includes
function(link_common_includes TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${LOCAL_INCLUDE}
        ${SPDLOG_INCLUDE}
        ${JSON_INCLUDE}
        ${GLM_INCLUDE}
    )
endfunction()

# Core Libraries
add_library(projectV-ecs STATIC ${CORE_SRC_DIR}/ecs.cpp)
link_common_includes(projectV-ecs)

add_library(projectV-math STATIC ${CORE_SRC_DIR}/math.cpp)
link_common_includes(projectV-math)

add_library(projectV-log STATIC ${CORE_SRC_DIR}/log.cpp)
link_common_includes(projectV-log)

# Graphics Libraries
add_library(projectV-fbo STATIC ${GRAPHICS_SRC_DIR}/fbo.cpp)
link_common_includes(projectV-fbo)

add_library(projectV-render STATIC ${GRAPHICS_SRC_DIR}/render.cpp)
link_common_includes(projectV-render)

add_library(projectV-scene STATIC ${GRAPHICS_SRC_DIR}/scene.cpp)
find_package(OpenMP REQUIRED)
target_link_libraries(projectV-scene PRIVATE OpenMP::OpenMP_CXX)
link_common_includes(projectV-scene)

add_library(projectV-shader STATIC ${GRAPHICS_SRC_DIR}/shader.cpp)
link_common_includes(projectV-shader)

add_library(projectV-uniforms STATIC ${GRAPHICS_SRC_DIR}/uniforms.cpp)
link_common_includes(projectV-uniforms)

add_library(projectV-user_input STATIC ${GRAPHICS_SRC_DIR}/user_input.cpp)
link_common_includes(projectV-user_input)

add_library(projectV-window STATIC ${GRAPHICS_SRC_DIR}/window.cpp)
link_common_includes(projectV-window)

# Utils Libraries
add_library(projectV-voxel_io STATIC ${UTILS_SRC_DIR}/voxel_io.cpp)
link_common_includes(projectV-voxel_io)

add_library(projectV-lod STATIC ${UTILS_SRC_DIR}/lod.cpp)
target_link_libraries(projectV-lod PRIVATE projectV-voxel_io)
link_common_includes(projectV-lod)

add_library(projectV-voxel_management STATIC ${UTILS_SRC_DIR}/voxel_management.cpp)
link_common_includes(projectV-voxel_management)

add_library(projectV-voxel_math STATIC ${UTILS_SRC_DIR}/voxel_math.cpp)
link_common_includes(projectV-voxel_math)
