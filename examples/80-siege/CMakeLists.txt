# Two programs from one folder, each staged with its own copy of the siege field: the same files, so
# the handshake's scene hash matches. The client draws with the examples' shared renderer
# (examples/shared/voxelRenderer) and one pass of its own after it, the HUD.
projv_add_example(siege_server
    SOURCES    server.cpp
    ASSET_DIRS siege
)
projv_add_example(siege_client
    SOURCES      client.cpp
    RENDERER     ../shared/voxelRenderer
    INCLUDE_DIRS ../shared
    SHADERS      hudShaders
    ASSET_DIRS   siege hudShaders
)
