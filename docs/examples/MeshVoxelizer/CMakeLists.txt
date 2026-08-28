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
projv_add_example(mesh_voxelizer
    SOURCES      main.cpp
    INCLUDE_DIRS include
    LIBRARIES    assimp zlibstatic Threads::Threads
)

target_include_directories(mesh_voxelizer PRIVATE
    # zlib, for the compressed NBT inside Minecraft region files. Assimp already vendors and
    # builds it, so this costs no additional dependency. zconf.h is generated into the build tree.
    "${ASSIMP_DIR}/contrib/zlib"
    "${CMAKE_BINARY_DIR}/assimp/contrib/zlib"
)
