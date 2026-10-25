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
#       RENDERER     ../shared/basicRenderer            # optional: renderer folder(s) several examples share
#   )
#
# A shared RENDERER folder holds render.json, resources.json, shaders/ and, if it has any, include/
# (its shader includes). Its shaders are compiled once, by a target of its own that every example using it waits
# on -- they are written beside their sources, so two examples compiling them would both claim the
# same outputs -- and the folder is staged beside each binary under its own name.
#
# Assets are staged beside the binary because the engine's load functions resolve relative
# paths against the working directory, and renderer folders name their shaders relative to
# it in resources.json. Running the example from its own build directory is what makes those
# paths resolve.

function(projv_add_example name)
    set(multiValueArgs SOURCES SHADERS SHADER_INCLUDE_DIRS ASSET_DIRS ASSET_FILES
                       INCLUDE_DIRS LIBRARIES RENDERER)
    cmake_parse_arguments(ARG "" "" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "projv_add_example(${name}): SOURCES is required")
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ProjectV::projectV ${ARG_LIBRARIES})

    # Examples do real work on the CPU -- procedural generation, voxelization, tree64 builds -- so
    # they get the same optimisation level as the engine. See the note in the top-level
    # CMakeLists.txt for why this is per target rather than a global flag.
    target_compile_options(${name} PRIVATE ${PROJV_OPTIMISED_BUILD_FLAGS})

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
        set(PROJV_STAGE_AFTER_SHADERS TRUE)
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

    # Staging runs POST_BUILD on the executable, which is only rebuilt when its *sources* change --
    # so a shader-only edit recompiles the .bin beside the source and then never copies it here,
    # leaving the running example on a stale shader with nothing to indicate it. Ordering the
    # shader target before the executable makes the relink (and therefore the staging) happen
    # whenever a shader changes.
    if(PROJV_STAGE_AFTER_SHADERS)
        add_dependencies(${name} ${name}_shaders)
        set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS
                     $<TARGET_PROPERTY:${name}_shaders,PROJV_SHADER_OUTPUTS>)
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

    # Each shared renderer is compiled once, by a target of its own, and staged beside every example
    # that names it. Its shaders see its own include/ and the include/ beside it (examples/shared/
    # include: what more than one renderer, or an example's own shaders, also include).
    foreach(rendererPath IN LISTS ARG_RENDERER)
        get_filename_component(rendererDir "${CMAKE_CURRENT_SOURCE_DIR}/${rendererPath}" ABSOLUTE)
        get_filename_component(rendererName "${rendererDir}" NAME)
        set(rendererTarget projv_renderer_${rendererName})
        if(NOT TARGET ${rendererTarget}_shaders)
            add_custom_target(${rendererTarget})   # what projv_compile_shaders hangs its shaders off
            set(rendererIncludeDirs "")
            if(EXISTS "${rendererDir}/include")
                list(APPEND rendererIncludeDirs "${rendererDir}/include")
            endif()
            get_filename_component(sharedIncludeDir "${rendererDir}/../include" ABSOLUTE)
            if(EXISTS "${sharedIncludeDir}")
                list(APPEND rendererIncludeDirs "${sharedIncludeDir}")
            endif()
            projv_compile_shaders(
                TARGET       ${rendererTarget}
                SHADER_DIRS  "${rendererDir}/shaders"
                INCLUDE_DIRS ${rendererIncludeDirs}
            )
        endif()
        add_dependencies(${name} ${rendererTarget}_shaders)
        set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS
                     $<TARGET_PROPERTY:${rendererTarget}_shaders,PROJV_SHADER_OUTPUTS>)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${rendererDir}" "${exampleBinaryDir}/${rendererName}"
            COMMENT "Staging ${rendererName} for ${name}"
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
