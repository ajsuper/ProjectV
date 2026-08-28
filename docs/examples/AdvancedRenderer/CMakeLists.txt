projv_add_example(advanced_renderer
    SOURCES             main.cpp
    SHADERS             advancedRenderer/cascadeShaders
    # The example's own light rig and cascade maths (pjv_sun_sky, pjv_cascade_*). The engine's
    # shader library is on the include path already; this is the example's half.
    SHADER_INCLUDE_DIRS sharedShaders
    ASSET_DIRS          advancedRenderer
)
