# 55-mountains: mountains in the style of the Sangre de Cristo at Santa Fe.
# Plan: docs/plans/mountains-santa-fe.md.

# mountains_lab: the landform in 2D (maps, profiles, a panorama from the Plaza) as PNGs.
add_executable(mountains_lab tools/lab.cpp)
target_include_directories(mountains_lab PRIVATE include)
target_link_libraries(mountains_lab PRIVATE bgfx::bimg bgfx::bx)
set_target_properties(mountains_lab PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/mountains")

# mountains: the clipmap of voxel rings, drawn with the engine's basic renderer (M2; its own renderer
# comes in M3).
projv_add_example(mountains
    SOURCES      main.cpp
    INCLUDE_DIRS include
)
