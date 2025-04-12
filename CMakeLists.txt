cmake_minimum_required(VERSION 3.10)
project(ProjectV)

# Compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O3")

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files
set(CORE_SRC ${CMAKE_SOURCE_DIR}/src/core.cpp)
set(GRAPHICS_SRC ${CMAKE_SOURCE_DIR}/src/graphics.cpp)
set(UTILS_SRC ${CMAKE_SOURCE_DIR}/src/utils.cpp)

# Output directories
set(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIBRARY_OUTPUT_PATH})

# Create static libraries
add_library(projectV-core STATIC ${CORE_SRC})
add_library(projectV-graphics STATIC ${GRAPHICS_SRC})
add_library(projectV-utils STATIC ${UTILS_SRC})
