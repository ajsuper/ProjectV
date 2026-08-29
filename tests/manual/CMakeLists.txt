# Manual tests. These need a display and a GPU, so they are opt-in: -DPROJV_BUILD_MANUAL_TESTS=ON.
#
# They are not examples. Nothing here is meant to be read as a demonstration of how to use the
# engine -- these are harnesses that exercise it and report what they found. Both were previously
# filed under the examples, which is why they carried READMEs written as if they were.
#
# exit_path and paths_probe are built by the hand-written Makefile beside them rather than by
# CMake, deliberately: exit_path exists partly to check the engine behaves for a consumer that is
# not using ProjectV's own CMake helpers.
include(ProjectVExample)

add_subdirectory(editing_p1)
add_subdirectory(edit_demo)
