# A city, a house or a forest blown apart -- real fracture and collapse in the physics, a mushroom cloud of
# glowing voxel embers -- drawn by a night-capable variant of the shared renderer (nightRenderer/: its own
# shaders and include/, compiled once and staged beside the binary).
projv_add_example(explosion
    SOURCES     main.cpp
    RENDERER    nightRenderer
    ASSET_FILES physics_materials.json
)
