projv_add_example(voxel_mayhem
    SOURCES             main.cpp
    SHADERS             mayhemRenderer/shaders
    # grr_common.sc: the camera maths, light rig and fog every pass shares; grr_giblur.sc: the GI blur.
    SHADER_INCLUDE_DIRS sharedShaders
    ASSET_DIRS          mayhemRenderer
    ASSET_FILES         physics_materials.json
)
