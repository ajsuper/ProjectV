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
    test_block_table.cpp
    test_tile_table.cpp
    test_color.cpp
    test_collision_geometry.cpp
    test_entities.cpp
    test_events.cpp
    test_input.cpp
    test_local_ids.cpp
    test_net.cpp
    test_loose_bvh.cpp
    test_physics_character.cpp
    test_physics_edits.cpp
    test_physics_fracture.cpp
    test_physics_golden.cpp
    test_physics_groups.cpp
    test_physics_contacts.cpp
    test_physics_kinematic.cpp
    test_physics_loading.cpp
    test_physics_joints.cpp
    test_physics_materials.cpp
    test_physics_net.cpp
    test_physics_prediction.cpp
    test_physics_queries.cpp
    test_physics_reliability.cpp
    test_replication.cpp
    test_physics_runtime.cpp
    test_physics_world.cpp
    test_slot_reuse.cpp
    test_scene_bridge.cpp
    test_scene_delete.cpp
    test_scene_loading.cpp
    test_world_loading.cpp
    test_time.cpp
    test_voxel_edits.cpp
)
target_link_libraries(projv_unit_tests PRIVATE projectV doctest::doctest)
# Committed test data: the golden physics recording (test_physics_golden.cpp).
target_compile_definitions(projv_unit_tests PRIVATE PROJV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(projv_unit_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
)

add_test(NAME projv_unit_tests COMMAND projv_unit_tests)

# The physics soak: thousands of bodies for minutes of simulated time. Not in the default run
# (`ctest --preset dev` excludes the label); `ctest --preset soak`, or set PROJV_SOAK yourself.
add_test(NAME physics_soak COMMAND projv_unit_tests -tc=soak*)
set_tests_properties(physics_soak PROPERTIES ENVIRONMENT "PROJV_SOAK=1" LABELS soak TIMEOUT 3600)

# Real sockets on localhost (ENet). Not in the default run either: `ctest --preset net`.
add_test(NAME net_sockets COMMAND projv_unit_tests -tc=enet* --no-skip)
set_tests_properties(net_sockets PROPERTIES LABELS net TIMEOUT 60)
# The multiplayer example as three processes (a server, two headless clients) on localhost.
if(TARGET mp_server AND TARGET mp_client)
    add_test(NAME multiplayer_selftest
             COMMAND ${PROJECT_SOURCE_DIR}/examples/70-multiplayer/selftest.sh ${CMAKE_BINARY_DIR})
    set_tests_properties(multiplayer_selftest PROPERTIES LABELS net TIMEOUT 120)
endif()
# A hundred bot clients against one server, as real sockets (examples/70-multiplayer/scale.sh).
if(TARGET mp_server AND TARGET mp_bots)
    add_test(NAME multiplayer_scale
             COMMAND ${PROJECT_SOURCE_DIR}/examples/70-multiplayer/scale.sh ${CMAKE_BINARY_DIR} 100)
    set_tests_properties(multiplayer_scale PROPERTIES LABELS net TIMEOUT 180)
endif()

# The party game as four processes (a server; three clients wandering, shoving, building, bombing).
if(TARGET party_demo_server AND TARGET party_demo)
    add_test(NAME party_selftest
             COMMAND ${PROJECT_SOURCE_DIR}/examples/90-party-demo/selftest.sh ${CMAKE_BINARY_DIR})
    set_tests_properties(party_selftest PROPERTIES LABELS net TIMEOUT 300)
endif()

# The playground as three processes (a server; two clients using every tool on the castle).
if(TARGET playground_demo_server AND TARGET playground_demo)
    add_test(NAME playground_selftest
             COMMAND ${PROJECT_SOURCE_DIR}/examples/95-playground-demo/selftest.sh ${CMAKE_BINARY_DIR})
    set_tests_properties(playground_selftest PROPERTIES LABELS net TIMEOUT 180)
endif()

# Not a compile: a scan of the voxel layer's includes. See check_layering.cmake.
add_test(NAME layering
         COMMAND ${CMAKE_COMMAND} -DPROJV_ROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_layering.cmake)
