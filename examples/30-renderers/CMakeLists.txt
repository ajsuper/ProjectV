# One host, three renderers (restir-pt, pt-reference, fast). Each is a folder under renderers/ holding
# render.json, resources.json and its shaders; the host switches between them from its panel.
#
# Every entry here is an *implementation example* of one technique, not a tuned production
# renderer -- they are meant to be read and compared, so none of them borrows another's tricks.
projv_add_example(renderer_gallery
    SOURCES             main.cpp
    SHADERS             renderers/pt-reference/shaders
                        renderers/fast/shaders
                        renderers/restir-pt/shaders
    # The example's own shader library: the sun/sky model and the path tracers' light. The engine's
    # pjv_scene.sc is on the include path already.
    SHADER_INCLUDE_DIRS sharedShaders
    INCLUDE_DIRS        include ../shared
    ASSET_DIRS          renderers SponzaScene QuadSponza
    ASSET_FILES         LDR_RGBA_7.png
)
