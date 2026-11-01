# Layering rules. The first, from the runtime plan (D10): the voxel layer -- utils and data_structures --
# must not depend on EnTT or on the runtime built on it, so that tools with no game loop can load,
# edit and save scenes. EnTT is linked PUBLIC, so the compiler would never notice a violation; this
# does. Run by ctest as `layering`.
#
#   cmake -DPROJV_ROOT=<repo> -P check_layering.cmake
if(NOT PROJV_ROOT)
    message(FATAL_ERROR "check_layering: PROJV_ROOT is not set")
endif()

file(GLOB_RECURSE files
    "${PROJV_ROOT}/include/utils/*" "${PROJV_ROOT}/include/data_structures/*"
    "${PROJV_ROOT}/src/utils/*" "${PROJV_ROOT}/include/net/*" "${PROJV_ROOT}/src/net/*")

set(forbidden "#[ \t]*include[ \t]*[<\"](entt/|core/world\\.h|core/application\\.h|core/events\\.h|core/ecs\\.h|runtime/)")
set(violations "")
foreach(file IN LISTS files)
    file(STRINGS "${file}" hits REGEX "${forbidden}")
    foreach(hit IN LISTS hits)
        file(RELATIVE_PATH relative "${PROJV_ROOT}" "${file}")
        string(APPEND violations "\n  ${relative}: ${hit}")
    endforeach()
endforeach()

if(violations)
    message(FATAL_ERROR "The voxel layer includes the runtime or EnTT:${violations}")
endif()
message(STATUS "layering: utils, data_structures and net are free of EnTT and the runtime")

# Jolt is reachable only from src/runtime/physics/. Those files are compiled apart, with Jolt's SIMD
# flags and configuration defines (see projv_physics in the top-level CMakeLists); a Jolt header
# anywhere else would be compiled without them, and no public header may name a Jolt type, so a
# consumer of the engine never needs Jolt's configuration to match its own.
file(GLOB_RECURSE everything
    "${PROJV_ROOT}/include/*" "${PROJV_ROOT}/src/*" "${PROJV_ROOT}/tests/unit/*"
    "${PROJV_ROOT}/examples/*.cpp" "${PROJV_ROOT}/examples/*.h")
set(jolt "#[ \t]*include[ \t]*[<\"]Jolt/")
set(jolt_violations "")
foreach(file IN LISTS everything)
    if(file MATCHES "^${PROJV_ROOT}/src/runtime/physics/" OR file MATCHES "/external/")
        continue()
    endif()
    file(STRINGS "${file}" hits REGEX "${jolt}")
    foreach(hit IN LISTS hits)
        file(RELATIVE_PATH relative "${PROJV_ROOT}" "${file}")
        string(APPEND jolt_violations "\n  ${relative}: ${hit}")
    endforeach()
endforeach()
if(jolt_violations)
    message(FATAL_ERROR "Jolt is included outside src/runtime/physics/:${jolt_violations}")
endif()
message(STATUS "layering: Jolt is included only by src/runtime/physics/")

# ENet is reachable only from src/net/transport_enet.cpp: everything else talks to net::Transport.
set(enet "#[ \t]*include[ \t]*[<\"]enet/")
set(enet_violations "")
foreach(file IN LISTS everything)
    if(file MATCHES "^${PROJV_ROOT}/src/net/transport_enet\\.cpp$" OR file MATCHES "/external/")
        continue()
    endif()
    file(STRINGS "${file}" hits REGEX "${enet}")
    foreach(hit IN LISTS hits)
        file(RELATIVE_PATH relative "${PROJV_ROOT}" "${file}")
        string(APPEND enet_violations "\n  ${relative}: ${hit}")
    endforeach()
endforeach()
if(enet_violations)
    message(FATAL_ERROR "ENet is included outside src/net/transport_enet.cpp:${enet_violations}")
endif()
message(STATUS "layering: ENet is included only by src/net/transport_enet.cpp")

# The UI (docs/plans/ui.md, D2). It sits on core and graphics, so nothing beneath it includes it. ImGui
# is reachable in the engine only from the dev layer (src/ui/dev/) and the one header that hands it to
# panel code (ui/dev.h); RmlUi only from the user layer (src/ui/user/), anywhere in the tree, so it can
# be replaced without touching a program.
file(GLOB_RECURSE beneathUi
    "${PROJV_ROOT}/include/core/*" "${PROJV_ROOT}/src/core/*"
    "${PROJV_ROOT}/include/graphics/*" "${PROJV_ROOT}/src/graphics/*"
    "${PROJV_ROOT}/include/utils/*" "${PROJV_ROOT}/src/utils/*"
    "${PROJV_ROOT}/include/data_structures/*"
    "${PROJV_ROOT}/include/net/*" "${PROJV_ROOT}/src/net/*")
file(GLOB_RECURSE engine "${PROJV_ROOT}/include/*" "${PROJV_ROOT}/src/*")
set(ui_violations "")
foreach(file IN LISTS beneathUi)
    file(STRINGS "${file}" hits REGEX "#[ \t]*include[ \t]*[<\"]ui/")
    foreach(hit IN LISTS hits)
        file(RELATIVE_PATH relative "${PROJV_ROOT}" "${file}")
        string(APPEND ui_violations "\n  ${relative} includes the UI: ${hit}")
    endforeach()
endforeach()
foreach(file IN LISTS engine)
    if(file MATCHES "^${PROJV_ROOT}/src/ui/dev/" OR file STREQUAL "${PROJV_ROOT}/include/ui/dev.h")
        continue()
    endif()
    file(STRINGS "${file}" hits REGEX "#[ \t]*include[ \t]*[<\"](imgui|backends/imgui)")
    foreach(hit IN LISTS hits)
        file(RELATIVE_PATH relative "${PROJV_ROOT}" "${file}")
        string(APPEND ui_violations "\n  ${relative} includes ImGui outside the dev layer: ${hit}")
    endforeach()
endforeach()
foreach(file IN LISTS everything)
    if(file MATCHES "^${PROJV_ROOT}/src/ui/user/" OR file MATCHES "/external/")
        continue()
    endif()
    file(STRINGS "${file}" hits REGEX "#[ \t]*include[ \t]*[<\"]RmlUi/")
    foreach(hit IN LISTS hits)
        file(RELATIVE_PATH relative "${PROJV_ROOT}" "${file}")
        string(APPEND ui_violations "\n  ${relative} includes RmlUi outside the user layer: ${hit}")
    endforeach()
endforeach()
if(ui_violations)
    message(FATAL_ERROR "UI layering:${ui_violations}")
endif()
message(STATUS "layering: nothing beneath ui/ includes it; ImGui only in the dev layer, RmlUi only in the user layer")
