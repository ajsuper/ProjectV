# Two programs from one folder, each staged with its own copy of the playground scene: the same files,
# so the handshake's scene hash matches. The client draws with the demos' shared renderer
# (examples/shared/voxelRenderer) and one pass of its own after it, the HUD.
projv_add_example(playground_demo_server
    SOURCES    server.cpp
    ASSET_DIRS playground
)
projv_add_example(playground_demo
    SOURCES      client.cpp
    RENDERER     ../shared/voxelRenderer
    INCLUDE_DIRS ../shared
    SHADERS      hudShaders
    ASSET_DIRS   playground hudShaders
)
