# One host, several renderers. Each is a folder under renderers/ holding render.json,
# resources.json and its shaders; the host picks between them with --renderer.
#
# Every entry here is an *implementation example* of one technique, not a tuned production
# renderer -- they are meant to be read and compared, so none of them borrows another's tricks.
projv_add_example(renderer_gallery
    SOURCES             main.cpp
    SHADERS             renderers/tree64/shaders
                        renderers/taa/shaders
                        renderers/fast/shaders
                        renderers/reconstruction/shaders
    # The example's own shader library: the sun/sky model and the cascade maths. The engine's
    # pjv_utils_DDA.sc is on the include path already.
    SHADER_INCLUDE_DIRS sharedShaders
    INCLUDE_DIRS        include
    ASSET_DIRS          renderers SponzaScene QuadSponza
    ASSET_FILES         LDR_RGBA_7.png
)
