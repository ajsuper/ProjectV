projv_add_example(terrain_generator
    SOURCES             main.cpp
    SHADERS             fastRenderer/pathTracerShaders
                        worldCascadeRenderer/pathTracerShaders
    SHADER_INCLUDE_DIRS sharedShaders
    INCLUDE_DIRS        include
    ASSET_DIRS          fastRenderer worldCascadeRenderer
    ASSET_FILES         LDR_RGBA_7.png
)
