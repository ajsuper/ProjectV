projv_add_example(voxel_mayhem_demo
    SOURCES             main.cpp
    # The demos' renderer, shared with the other demos (examples/shared/voxelRenderer).
    RENDERER            ../shared/voxelRenderer
    INCLUDE_DIRS        ../shared   # voxel_renderer.h
    ASSET_FILES         physics_materials.json
)
