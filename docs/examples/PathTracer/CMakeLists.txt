# Seven renderer folders, one host. Note that radianceCascadeRenderer is built here but is
# not reachable from the example's own menu, which offers six -- see the rehost planned for
# this example.
projv_add_example(path_tracer
    SOURCES             main.cpp
    SHADERS             tree64Renderer/pathTracerShaders
                        reprojectionRenderer/pathTracerShaders
                        fastRenderer/pathTracerShaders
                        faceRenderer/pathTracerShaders
                        radianceCascadeRenderer/pathTracerShaders
                        worldCascadeRenderer/pathTracerShaders
                        worldFaceCascadeRenderer/pathTracerShaders
    SHADER_INCLUDE_DIRS sharedShaders
    INCLUDE_DIRS        include
    ASSET_DIRS          tree64Renderer reprojectionRenderer fastRenderer faceRenderer
                        radianceCascadeRenderer worldCascadeRenderer worldFaceCascadeRenderer
                        SponzaScene QuadSponza
    ASSET_FILES         LDR_RGBA_7.png
)
