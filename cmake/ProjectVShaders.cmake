# projv_compile_shaders() -- compile a renderer's shaders with bgfx's shaderc.
#
# This replaces the twelve near-identical comp*.sh scripts that used to live one per example
# (two pairs of which were byte-for-byte duplicates across different examples). Each one
# re-derived the platform and profile from $OSTYPE, hardcoded a path to shadercRelease, and
# hand-listed the same include roots.
#
# Output is written *flat*, beside the sources, as <name>.bin -- deliberately not bgfx's own
# bgfx_compile_shaders(), which writes to <output>/<profile>/<name>.bin. Renderer folders name
# their shaders in resources.json ("./previewRenderer/previewShaders/albedo.bin"), so a
# per-profile subdirectory would mean rewriting the path in every resources.json in the tree.
#
#   projv_compile_shaders(
#       TARGET       path_tracer              # shaders build before this target
#       SHADER_DIRS  tree64Renderer/pathTracerShaders fastRenderer/pathTracerShaders ...
#       [INCLUDE_DIRS dir...]                 # extra -i roots, e.g. an example's sharedShaders
#       [PROFILE     spirv]                   # defaults per platform
#   )
#
# All of a target's shader directories go in one call. An example with seven renderer folders
# still gets one <target>_shaders custom target, rather than seven that collide on the name.
#
# Convention, matching every renderer in the tree: vs_*.sc are vertex shaders, *.frag are
# fragment shaders, and any other .sc is a shared include compiled by nobody.

function(projv_compile_shaders)
    set(oneValueArgs TARGET PROFILE)
    set(multiValueArgs SHADER_DIRS INCLUDE_DIRS)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "projv_compile_shaders: TARGET is required")
    endif()
    if(NOT ARG_SHADER_DIRS)
        message(FATAL_ERROR "projv_compile_shaders: SHADER_DIRS is required")
    endif()

    _projv_find_shaderc(SHADERC_EXECUTABLE)

    if(ARG_PROFILE)
        set(profile ${ARG_PROFILE})
        if(APPLE)
            set(platform osx)
        elseif(WIN32)
            set(platform windows)
        else()
            set(platform linux)
        endif()
    elseif(APPLE)
        set(platform osx)
        set(profile metal)
    elseif(WIN32)
        set(platform windows)
        set(profile spirv)
    else()
        set(platform linux)
        set(profile spirv)
    endif()

    # Include roots. bgfx's own bgfx_shader.sh, then the engine's shader library so
    # pjv_utils_DDA.sc resolves, then whatever the caller adds.
    set(includeFlags "")
    _projv_bgfx_shader_include(bgfxShaderInclude)
    if(bgfxShaderInclude)
        list(APPEND includeFlags -i "${bgfxShaderInclude}")
    endif()
    _projv_engine_shader_include(engineShaderInclude)
    if(engineShaderInclude)
        list(APPEND includeFlags -i "${engineShaderInclude}")
    endif()
    foreach(dir IN LISTS ARG_INCLUDE_DIRS)
        get_filename_component(dir "${dir}" ABSOLUTE)
        list(APPEND includeFlags -i "${dir}")
    endforeach()

    set(allOutputs "")
    foreach(rawShaderDir IN LISTS ARG_SHADER_DIRS)
        get_filename_component(shaderDir "${rawShaderDir}" ABSOLUTE)

        file(GLOB vertexShaders "${shaderDir}/vs_*.sc")
        file(GLOB fragmentShaders "${shaderDir}/*.frag")

        if(NOT vertexShaders AND NOT fragmentShaders)
            message(FATAL_ERROR
                "projv_compile_shaders: no vs_*.sc or *.frag found in ${shaderDir}")
        endif()

        # A shared .sc include has no $input/$output and cannot be compiled on its own, but
        # every shader that includes it must rebuild when it changes.
        file(GLOB sharedIncludes "${shaderDir}/*.sc")
        if(vertexShaders)
            list(REMOVE_ITEM sharedIncludes ${vertexShaders})
        endif()

        foreach(shader IN LISTS vertexShaders fragmentShaders)
            if(shader IN_LIST vertexShaders)
                set(shaderType v)
            else()
                set(shaderType f)
            endif()
            get_filename_component(shaderName "${shader}" NAME_WE)
            set(output "${shaderDir}/${shaderName}.bin")

            add_custom_command(
                OUTPUT "${output}"
                COMMAND "${SHADERC_EXECUTABLE}"
                        -f "${shader}" -o "${output}" --type ${shaderType}
                        --platform ${platform} --profile ${profile}
                        ${includeFlags}
                DEPENDS "${shader}" ${sharedIncludes}
                COMMENT "shaderc ${shaderName} (${platform}/${profile})"
                VERBATIM
            )
            list(APPEND allOutputs "${output}")
        endforeach()
    endforeach()

    add_custom_target(${ARG_TARGET}_shaders DEPENDS ${allOutputs})
    add_dependencies(${ARG_TARGET} ${ARG_TARGET}_shaders)

    # Published so the example helper can make the executable's link depend on the compiled shaders.
    # Without that, a shader-only edit rebuilds the .bin but never re-runs the POST_BUILD staging
    # step, and the example keeps loading the previously staged copy.
    set_property(TARGET ${ARG_TARGET}_shaders PROPERTY PROJV_SHADER_OUTPUTS "${allOutputs}")
endfunction()

# --- helpers -------------------------------------------------------------------------

# shaderc arrives three different ways: as a CMake target when bgfx.cmake is in the build,
# as an installed tool from vcpkg (tools/bgfx/shaderc), or on PATH.
function(_projv_find_shaderc outVar)
    if(TARGET bgfx::shaderc)
        set(${outVar} "$<TARGET_FILE:bgfx::shaderc>" PARENT_SCOPE)
        return()
    endif()
    if(TARGET shaderc)
        set(${outVar} "$<TARGET_FILE:shaderc>" PARENT_SCOPE)
        return()
    endif()
    find_program(PROJV_SHADERC_EXECUTABLE
        NAMES shaderc shadercRelease
        HINTS "${bgfx_DIR}/../../../tools/bgfx" "${CMAKE_PREFIX_PATH}"
        PATH_SUFFIXES tools/bgfx bin
    )
    if(NOT PROJV_SHADERC_EXECUTABLE)
        message(FATAL_ERROR
            "projv_compile_shaders: shaderc not found. Install bgfx with its tools "
            "(vcpkg: `vcpkg install bgfx[tools]`), or build ProjectV with the bgfx.cmake "
            "submodule, which builds shaderc itself.")
    endif()
    set(${outVar} "${PROJV_SHADERC_EXECUTABLE}" PARENT_SCOPE)
endfunction()

# bgfx's own shader headers (bgfx_shader.sh), which every shader includes.
function(_projv_bgfx_shader_include outVar)
    set(${outVar} "" PARENT_SCOPE)

    # bgfx_shader.sh sits in bgfx's sources in a build tree and beside its headers once
    # installed, and the two layouts differ again between an installed bgfx and a vcpkg one --
    # so ask the target where its headers are before falling back to guessing.
    set(candidates "${BGFX_DIR}/src")
    if(TARGET bgfx::bgfx)
        get_target_property(interfaceDirs bgfx::bgfx INTERFACE_INCLUDE_DIRECTORIES)
        if(interfaceDirs)
            foreach(dir IN LISTS interfaceDirs)
                string(REGEX REPLACE "^\\$<[A-Z_]+:(.+)>$" "\\1" dir "${dir}")
                list(APPEND candidates "${dir}" "${dir}/bgfx")
            endforeach()
        endif()
    endif()
    list(APPEND candidates
        "${CMAKE_CURRENT_LIST_DIR}/../external/bgfx/src"
        "${bgfx_DIR}/../../../include/bgfx"
    )

    foreach(candidate IN LISTS candidates)
        if(EXISTS "${candidate}/bgfx_shader.sh")
            get_filename_component(resolved "${candidate}" ABSOLUTE)
            set(${outVar} "${resolved}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    message(WARNING
        "projv_compile_shaders: could not locate bgfx_shader.sh. Shaders will fail to compile.")
endfunction()

# The engine's own shader library -- pjv_utils_DDA.sc. It ships inside the same directory the
# C++ headers are found through, so ask the target rather than guessing: that is include/ in a
# build tree and <prefix>/include/projv in an install tree, and the target knows which.
function(_projv_engine_shader_include outVar)
    set(${outVar} "" PARENT_SCOPE)

    set(candidates "")
    if(TARGET ProjectV::projectV)
        get_target_property(interfaceDirs ProjectV::projectV INTERFACE_INCLUDE_DIRECTORIES)
        if(interfaceDirs)
            foreach(dir IN LISTS interfaceDirs)
                # Strip the $<BUILD_INTERFACE:...> / $<INSTALL_INTERFACE:...> wrappers a build-tree
                # target carries; an imported target's are already resolved to plain paths.
                string(REGEX REPLACE "^\\$<[A-Z_]+:(.+)>$" "\\1" dir "${dir}")
                list(APPEND candidates "${dir}")
            endforeach()
        endif()
    endif()
    # Fallbacks for a caller that has not imported the target yet.
    list(APPEND candidates
        "${CMAKE_CURRENT_LIST_DIR}/../include"
        "${CMAKE_CURRENT_LIST_DIR}/../../../include/projv"
    )

    foreach(candidate IN LISTS candidates)
        if(EXISTS "${candidate}/pjv_utils_DDA.sc")
            get_filename_component(resolved "${candidate}" ABSOLUTE)
            set(${outVar} "${resolved}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    message(WARNING
        "projv_compile_shaders: could not locate the engine's shader library "
        "(pjv_utils_DDA.sc). Any shader that includes it will fail to compile.")
endfunction()
