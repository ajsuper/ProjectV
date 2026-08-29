set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/external/imgui")
set(LUA_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/external/lua")

# ImGui, docking branch, plus its GLFW backend. The bgfx backend is this example's own
# (imgui_impl_bgfx.cpp) because ImGui does not ship one.
add_library(scene_editor_imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
)
target_include_directories(scene_editor_imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(scene_editor_imgui PUBLIC ${PROJV_GLFW_TARGET})

# Lua, for the Brush Lab's programmable brushes.
#
# LUA_USE_POSIX rather than LUA_USE_LINUX, deliberately: the Linux preset adds LUA_USE_DLOPEN,
# which hands a brush script package.loadlib and with it the ability to load arbitrary native
# code. A brush is data, and should not be able to do that.
#
# Globbed rather than listed so a Lua point release does not need this file edited, minus the
# three sources that are not part of the library: lua.c is the standalone interpreter's main(),
# onelua.c is the single-file amalgamation, and ltests.c is the internal test harness.
file(GLOB LUA_SOURCES "${LUA_DIR}/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|onelua|ltests)\\.c$")

add_library(scene_editor_lua STATIC ${LUA_SOURCES})
target_include_directories(scene_editor_lua PUBLIC "${LUA_DIR}")
target_compile_definitions(scene_editor_lua PRIVATE LUA_USE_POSIX)
target_compile_options(scene_editor_lua PRIVATE -w) # Third-party C; not our warnings to fix.

projv_add_example(scene_editor
    SOURCES    main.cpp imgui_impl_bgfx.cpp edit_history.cpp brush.cpp
    SHADERS    editorRenderer/editorShaders
               editorRenderer/imguiShaders
               renderRenderer/renderShaders
    ASSET_DIRS editorRenderer renderRenderer brushes
    LIBRARIES  scene_editor_imgui scene_editor_lua
)
