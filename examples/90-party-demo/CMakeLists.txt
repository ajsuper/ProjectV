# Two programs from one folder, each staged with its own copy of the party scene: the same files, so
# the handshake's scene hash matches. The client draws with the demos' shared renderer
# (examples/shared/voxelRenderer); its HUD and menu are documents (ui/, and the shared menu).
projv_add_example(party_demo_server
    SOURCES    server.cpp
    ASSET_DIRS party
)
projv_add_example(party_demo
    SOURCES      client.cpp
    RENDERER     ../shared/voxelRenderer
    INCLUDE_DIRS ../shared
    ASSET_DIRS   party ui
    SHARED_UI
)
