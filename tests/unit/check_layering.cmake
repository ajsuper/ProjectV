# Two layering rules. The first, from the runtime plan (D10): the voxel layer -- utils and data_structures --
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
    "${PROJV_ROOT}/src/utils/*")

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
message(STATUS "layering: utils and data_structures are free of EnTT and the runtime")

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
