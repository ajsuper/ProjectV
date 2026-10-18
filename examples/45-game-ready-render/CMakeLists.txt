projv_add_example(game_ready_render
    SOURCES             main.cpp
    SHADERS             gameReadyRenderer/shaders
    # grr_common.sc: the camera maths, light rig and fog every pass shares.
    SHADER_INCLUDE_DIRS sharedShaders
    ASSET_DIRS          gameReadyRenderer
)
