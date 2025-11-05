cmake_minimum_required(VERSION 3.10)
project(ProjectV)

include(ExternalProject)

# Compiler settings
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O3")

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
set(BGFX_INCLUDE ${CMAKE_SOURCE_DIR}/external/bgfx/include)
set(BX_INCLUDE ${CMAKE_SOURCE_DIR}/external/bx/include)

# Header include paths (local)
set(LOCAL_INCLUDE ${CMAKE_SOURCE_DIR}/include)

# Source directories
set(CORE_SRC_DIR ${CMAKE_SOURCE_DIR}/src/core)
set(GRAPHICS_SRC_DIR ${CMAKE_SOURCE_DIR}/src/graphics)
set(UTILS_SRC_DIR ${CMAKE_SOURCE_DIR}/src/utils)

# External directory
set(EXTERNAL_DIR ${CMAKE_SOURCE_DIR}/external)

# Macro for linking includes
function(link_common_includes TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${LOCAL_INCLUDE}
        ${SPDLOG_INCLUDE}
        ${JSON_INCLUDE}
        ${GLM_INCLUDE}
        ${BGFX_INCLUDE}
        ${BX_INCLUDE}
    )
endfunction()

# bgfx, bx, and bimg
if(APPLE)
    set(BGFX_MAKE_TARGET "osx-arm64")

    # Add macOS system frameworks
    set(MACOS_FRAMEWORKS
        "-framework Cocoa"
        "-framework Metal"
        "-framework IOKit"
        "-framework QuartzCore"
    )
elseif(WIN32)
    set(BGFX_MAKE_TARGET "mingw-gcc")
else()
    set(BGFX_MAKE_TARGET "linux-gcc")
endif()

# Build bgfx with it's make file
ExternalProject_Add(
    bgfx_build
    SOURCE_DIR ${EXTERNAL_DIR}/bgfx
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make -C ${EXTERNAL_DIR}/bgfx ${BGFX_MAKE_TARGET} -j4
    BUILD_IN_SOURCE 1
    INSTALL_COMMAND ""
)

# Create imported library targets
add_library(bgfx STATIC IMPORTED)
add_library(bx STATIC IMPORTED)
add_library(bimg STATIC IMPORTED)

# Set compiled bgfx library location
set_target_properties(bgfx PROPERTIES
    IMPORTED_LOCATION ${EXTERNAL_DIR}/bgfx/.build/osx64_clang/bin/libbgfxRelease.a
    INTERFACE_INCLUDE_DIRECTORIES ${EXTERNAL_DIR}/bgfx/include
)

add_dependencies(bgfx bgfx_build)

# Core Libraries
add_library(projectV-ecs STATIC ${CORE_SRC_DIR}/ecs.cpp)
link_common_includes(projectV-ecs)

add_library(projectV-math STATIC ${CORE_SRC_DIR}/math.cpp)
link_common_includes(projectV-math)

# Graphics Libraries
add_library(projectV-render_instance STATIC ${GRAPHICS_SRC_DIR}/render_instance.cpp)
target_link_libraries(projectV-render_instance PRIVATE bgfx glfw ${MACOS_FRAMEWORKS})
link_common_includes(projectV-render_instance)

add_library(projectV-disk_io STATIC ${GRAPHICS_SRC_DIR}/disk_io.cpp)
target_link_libraries(projectV-disk_io PRIVATE bgfx glfw ${MACOS_FRAMEWORKS} projectV-type_mapping)
link_common_includes(projectV-disk_io)

add_library(projectV-gpu_interface STATIC ${GRAPHICS_SRC_DIR}/gpu_interface.cpp)
target_link_libraries(projectV-gpu_interface PRIVATE bgfx glfw ${MACOS_FRAMEWORKS})
link_common_includes(projectV-gpu_interface)

add_library(projectV-manage_resources STATIC ${GRAPHICS_SRC_DIR}/manage_resources.cpp)
target_link_libraries(projectV-manage_resources PRIVATE bgfx glfw ${MACOS_FRAMEWORKS} projectV-type_mapping projectV-disk_io)
link_common_includes(projectV-manage_resources)

add_library(projectV-perform_renderer STATIC ${GRAPHICS_SRC_DIR}/perform_renderer.cpp)
target_link_libraries(projectV-perform_renderer PRIVATE bgfx glfw ${MACOS_FRAMEWORKS} projectV-manage_resources)
link_common_includes(projectV-perform_renderer)

add_library(projectV-type_mapping STATIC ${GRAPHICS_SRC_DIR}/type_mapping.cpp)
target_link_libraries(projectV-type_mapping PRIVATE bgfx glfw ${MACOS_FRAMEWORKS} )
link_common_includes(projectV-type_mapping)

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
