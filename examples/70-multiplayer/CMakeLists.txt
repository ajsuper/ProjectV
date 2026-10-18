# Two programs from one folder, each staged with its own copy of the arena: the same files, so the
# handshake's scene hash matches.
projv_add_example(mp_server
    SOURCES    server.cpp
    ASSET_DIRS arena
)
projv_add_example(mp_client
    SOURCES      client.cpp
    RENDERER     ../shared/voxelRenderer
    INCLUDE_DIRS ../shared
    ASSET_DIRS   arena
)
projv_add_example(mp_bots
    SOURCES    bots.cpp
    ASSET_DIRS arena
)
