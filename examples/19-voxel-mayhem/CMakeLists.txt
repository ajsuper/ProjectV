projv_add_example(voxel_mayhem
    SOURCES             main.cpp
    # The ten-pass renderer, shared with other examples (examples/shared/voxelRenderer).
    RENDERER            ../shared/voxelRenderer
    INCLUDE_DIRS        ../shared   # voxel_renderer.h
    ASSET_FILES         physics_materials.json
)
