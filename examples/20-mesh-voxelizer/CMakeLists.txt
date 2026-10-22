# The voxelizer needs Assimp -- the multi-format mesh importer this tool leans on -- which is a
# CMake project. Building it as a subproject is the only mesh-import story that is identical on
# Linux, macOS and Windows; system packages differ per distro and do not exist at all on Windows.

set(ASSIMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/assimp")

# --- Assimp ---------------------------------------------------------------------------------
# Built static and stripped to what a voxelizer needs: importers only (nothing is ever exported),
# no tools, no samples, no tests. The bundled zlib is used so nothing has to be installed system
# wide -- several container formats (glTF-binary, compressed FBX, Blender) need it.
set(BUILD_SHARED_LIBS            OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES         OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS    OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_VIEW     OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL               OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT             ON  CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS    OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB            ON  CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX  OFF CACHE BOOL "" FORCE)

add_subdirectory("${ASSIMP_DIR}" "${CMAKE_BINARY_DIR}/assimp" EXCLUDE_FROM_ALL)

# std::thread needs the platform's threading library explicitly on Linux; the voxelization loop
# runs one worker per core.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Headless: it writes a .data container and never opens a window. It still links the whole engine,
# which costs nothing at link time for the parts it does not call and spares this file a
# hand-maintained list of engine libraries -- the list that went stale when compose_io started
# reaching for addComponent/setComponentTransform.
# --- Teardown levels --------------------------------------------------------------------------
# teardown/ reads Teardown's XML level files (pugixml, vendored in include/pugixml) and runs their
# voxscripts -- the Lua that draws trees, windows and terrain -- on the Lua the scene editor
# already vendors. Without that submodule the Teardown input still builds and simply skips them.
set(TEARDOWN_SOURCES
    teardown/teardown_import.cpp
    teardown/discover.cpp
    teardown/vox_file.cpp
    teardown/materials.cpp
    teardown/canvas.cpp
    teardown/shapes.cpp
    teardown/voxscript.cpp
    teardown/level.cpp
    teardown/writer.cpp
    teardown/preview.cpp
    teardown/png.cpp
    teardown/verify.cpp
    include/pugixml/pugixml.cpp
)

set(TEARDOWN_LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../60-scene-editor/external/lua")
set(TEARDOWN_LUA_LIBRARY "")
if(EXISTS "${TEARDOWN_LUA_DIR}/lua.h")
    file(GLOB TEARDOWN_LUA_SOURCES "${TEARDOWN_LUA_DIR}/*.c")
    # Not part of the library: the standalone interpreter, the amalgamation, the test harness.
    list(FILTER TEARDOWN_LUA_SOURCES EXCLUDE REGEX "/(lua|onelua|ltests)\\.c$")
    add_library(mesh_voxelizer_lua STATIC ${TEARDOWN_LUA_SOURCES})
    target_include_directories(mesh_voxelizer_lua PUBLIC "${TEARDOWN_LUA_DIR}")
    target_compile_definitions(mesh_voxelizer_lua PRIVATE LUA_USE_POSIX)
    target_compile_options(mesh_voxelizer_lua PRIVATE -w) # Third-party C; not our warnings to fix.
    set(TEARDOWN_LUA_LIBRARY mesh_voxelizer_lua)
else()
    message(STATUS "mesh_voxelizer: Lua not found at ${TEARDOWN_LUA_DIR}; Teardown voxscripts will be skipped")
endif()

projv_add_example(mesh_voxelizer
    SOURCES      main.cpp ${TEARDOWN_SOURCES}
    INCLUDE_DIRS include include/pugixml teardown
    LIBRARIES    assimp zlibstatic Threads::Threads ${TEARDOWN_LUA_LIBRARY}
)
if(TEARDOWN_LUA_LIBRARY)
    target_compile_definitions(mesh_voxelizer PRIVATE PROJV_TEARDOWN_LUA)
endif()

target_include_directories(mesh_voxelizer PRIVATE
    # zlib, for the compressed NBT inside Minecraft region files. Assimp already vendors and
    # builds it, so this costs no additional dependency. zconf.h is generated into the build tree.
    "${ASSIMP_DIR}/contrib/zlib"
    "${CMAKE_BINARY_DIR}/assimp/contrib/zlib"
)
