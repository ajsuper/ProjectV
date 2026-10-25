projv_add_example(voxel_mayhem_demo
    SOURCES             main.cpp
    # The demos' renderer, shared with the other demos (examples/shared/voxelRenderer): MAYHEM_RENDERER=voxel.
    RENDERER            ../shared/voxelRenderer
    INCLUDE_DIRS        ../shared   # voxel_renderer.h, restir_pt.h
    ASSET_FILES         physics_materials.json
)

# The main renderer: the renderer gallery's restir-pt, compiled by the gallery's shader target and staged
# here as renderers/restir-pt/ -- where its resources.json names its shaders (examples/shared/restir_pt.h).
set(restirSource "${CMAKE_CURRENT_SOURCE_DIR}/../30-renderers/renderers/restir-pt")
add_dependencies(voxel_mayhem_demo renderer_gallery_shaders)
set_property(TARGET voxel_mayhem_demo APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:renderer_gallery_shaders,PROJV_SHADER_OUTPUTS>)
add_custom_command(TARGET voxel_mayhem_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${restirSource}" "${CMAKE_BINARY_DIR}/examples/voxel_mayhem_demo/renderers/restir-pt"
    COMMENT "Staging restir-pt for voxel_mayhem_demo"
    VERBATIM
)
