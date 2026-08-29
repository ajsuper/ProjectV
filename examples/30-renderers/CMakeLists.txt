# Seven renderers, one host. Each is a folder under renderers/ holding render.json,
# resources.json and its shaders; the host picks between them with --renderer.
projv_add_example(renderer_gallery
    SOURCES             main.cpp
    SHADERS             renderers/tree64/shaders
                        renderers/reprojection/shaders
                        renderers/fast/shaders
                        renderers/face/shaders
                        renderers/radiance-cascade/shaders
                        renderers/world-cascade/shaders
                        renderers/world-face-cascade/shaders
    # The example's own shader library: the sun/sky model and the cascade maths. The engine's
    # pjv_utils_DDA.sc is on the include path already.
    SHADER_INCLUDE_DIRS sharedShaders
    INCLUDE_DIRS        include
    ASSET_DIRS          renderers SponzaScene QuadSponza
    ASSET_FILES         LDR_RGBA_7.png
)
