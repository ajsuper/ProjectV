# Unit tests: pure CPU, no window and no GPU, so they run anywhere ctest does.
#
#   cmake --preset dev && cmake --build --preset dev && ctest --preset dev
#
# doctest is found as a package first and taken from the submodule otherwise, the same order every
# other dependency follows. It is header-only, so the submodule path names its header directory
# rather than running its own CMake build.
find_package(doctest CONFIG QUIET)
if(NOT TARGET doctest::doctest)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/doctest/doctest/doctest.h")
        message(FATAL_ERROR "PROJV_BUILD_TESTS needs doctest: run "
                            "`git submodule update --init external/doctest`, or install it.")
    endif()
    add_library(projv_doctest INTERFACE)
    target_include_directories(projv_doctest SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/external/doctest")
    add_library(doctest::doctest ALIAS projv_doctest)
endif()

add_executable(projv_unit_tests
    main.cpp
    test_application.cpp
    test_attachments.cpp
    test_entities.cpp
    test_events.cpp
    test_input.cpp
    test_local_ids.cpp
    test_loose_bvh.cpp
    test_scene_bridge.cpp
    test_scene_delete.cpp
    test_time.cpp
)
target_link_libraries(projv_unit_tests PRIVATE projectV doctest::doctest)
target_compile_options(projv_unit_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
)

add_test(NAME projv_unit_tests COMMAND projv_unit_tests)

# Not a compile: a scan of the voxel layer's includes. See check_layering.cmake.
add_test(NAME layering
         COMMAND ${CMAKE_COMMAND} -DPROJV_ROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_layering.cmake)
