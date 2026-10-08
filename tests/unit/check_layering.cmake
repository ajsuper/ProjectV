# The layering rule from the runtime plan (D10): the voxel layer -- utils and data_structures --
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
