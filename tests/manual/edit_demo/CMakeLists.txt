# Interactive editing on the GPU. Borrows its shaders from elsewhere; no renderer folder of its own.
projv_add_example(edit_demo
    SOURCES      main.cpp
    INCLUDE_DIRS include
    ASSET_DIRS   SponzaScene
)
