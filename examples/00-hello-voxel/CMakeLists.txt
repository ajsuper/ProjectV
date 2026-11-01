projv_add_example(hello_voxel
    SOURCES    main.cpp
    # Its renderer, as a folder: render.json, resources.json and the shader, staged beside the binary.
    SHADERS    renderer/shaders
    ASSET_DIRS renderer
)
