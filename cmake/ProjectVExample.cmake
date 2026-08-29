# projv_add_example() -- declare one bundled example.
#
# Every example used to carry a hand-written Makefile that re-derived the same eight -I
# paths, the same -L, and a hand-ordered list of sixteen -lprojectV-* archives wrapped (or,
# in four of seven cases, not wrapped) in -Wl,--start-group. Linking ProjectV::projectV
# replaces all of it.
#
#   projv_add_example(scene_previewer
#       SOURCES      main.cpp
#       SHADERS      previewRenderer/previewShaders     # optional, repeatable
#       SHADER_INCLUDE_DIRS sharedShaders               # optional
#       ASSET_DIRS   previewRenderer scenes             # optional
#       ASSET_FILES  LDR_RGBA_7.png                     # optional
#       INCLUDE_DIRS include                            # optional
#       LIBRARIES    ...                                # optional extra deps
#   )
#
# Assets are staged beside the binary because the engine's load functions resolve relative
# paths against the working directory, and renderer folders name their shaders relative to
# it in resources.json. Running the example from its own build directory is what makes those
# paths resolve.

function(projv_add_example name)
    set(multiValueArgs SOURCES SHADERS SHADER_INCLUDE_DIRS ASSET_DIRS ASSET_FILES
                       INCLUDE_DIRS LIBRARIES)
    cmake_parse_arguments(ARG "" "" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "projv_add_example(${name}): SOURCES is required")
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ProjectV::projectV ${ARG_LIBRARIES})

    foreach(dir IN LISTS ARG_INCLUDE_DIRS)
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
    endforeach()

    # Each example gets its own directory under the build tree, so its staged assets and its
    # sibling examples' cannot collide.
    set(exampleBinaryDir "${CMAKE_BINARY_DIR}/examples/${name}")
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${exampleBinaryDir}")

    if(ARG_SHADERS)
        # One call for all of an example's shader directories; projv_compile_shaders makes a
        # single <name>_shaders target from them.
        set(shaderDirs "")
        foreach(shaderDir IN LISTS ARG_SHADERS)
            list(APPEND shaderDirs "${CMAKE_CURRENT_SOURCE_DIR}/${shaderDir}")
        endforeach()
        set(shaderIncludeDirs "")
        foreach(includeDir IN LISTS ARG_SHADER_INCLUDE_DIRS)
            list(APPEND shaderIncludeDirs "${CMAKE_CURRENT_SOURCE_DIR}/${includeDir}")
        endforeach()
        projv_compile_shaders(
            TARGET       ${name}
            SHADER_DIRS  ${shaderDirs}
            INCLUDE_DIRS ${shaderIncludeDirs}
        )
    endif()

    # Staged after the build so freshly compiled .bin shaders are copied along with the
    # renderer JSON that names them.
    foreach(assetDir IN LISTS ARG_ASSET_DIRS)
        # The staged copy would land on top of the executable itself, which sits at the root of
        # the same directory. CMake reports this only as "Error copying directory", so name it.
        if(assetDir STREQUAL name)
            message(FATAL_ERROR
                "projv_add_example(${name}): ASSET_DIRS contains \"${assetDir}\", which is also "
                "the target name -- staging it would collide with the executable. Rename one.")
        endif()
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/${assetDir}"
                    "${exampleBinaryDir}/${assetDir}"
            COMMENT "Staging ${assetDir} for ${name}"
            VERBATIM
        )
    endforeach()

    foreach(assetFile IN LISTS ARG_ASSET_FILES)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/${assetFile}"
                    "${exampleBinaryDir}/${assetFile}"
            COMMENT "Staging ${assetFile} for ${name}"
            VERBATIM
        )
    endforeach()
endfunction()
