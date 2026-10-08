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
    test_attachments.cpp
)
target_link_libraries(projv_unit_tests PRIVATE projectV doctest::doctest)
target_compile_options(projv_unit_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
)

add_test(NAME projv_unit_tests COMMAND projv_unit_tests)
