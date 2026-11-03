# The island demo (docs/plans/island-demo.md): a generated island to break, gather and build with. It
# draws with the renderer gallery's restir-pt, staged beside it as Voxel Mayhem's is.
projv_add_example(island_demo
    SOURCES      main.cpp
    INCLUDE_DIRS ../shared   # restir_pt.h
    ASSET_FILES  physics_materials.json
    ASSET_DIRS   ui         # the HUD, the start screen and the menu, their style and font
)

set(restirSource "${CMAKE_CURRENT_SOURCE_DIR}/../30-renderers/renderers/restir-pt")
add_dependencies(island_demo renderer_gallery_shaders)
set_property(TARGET island_demo APPEND PROPERTY LINK_DEPENDS $<TARGET_PROPERTY:renderer_gallery_shaders,PROJV_SHADER_OUTPUTS>)
add_custom_command(TARGET island_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${restirSource}" "${CMAKE_BINARY_DIR}/examples/island_demo/renderers/restir-pt"
    COMMENT "Staging restir-pt for island_demo"
    VERBATIM
)
