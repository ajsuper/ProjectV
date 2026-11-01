# The shader library is checked (docs/plans/shader-library.md, D7). Run as a script by ctest:
#
#   cmake -DSHADERC=... -DBGFX_SHADER_INCLUDE=... -DLIBRARY=shaders/include -DVARYING=... -DWORK=... -P check_shader_library.cmake
#
# 1. Every file in the library compiles on its own -- it includes what it needs -- and all of them
#    together, for each profile the tree builds or may: SPIR-V, GLSL 440 and Metal. (HLSL needs the
#    Windows compiler, so it is not checked here.)
# 2. A scan: no `uniform` or sampler outside pjv_engine.sc, and every symbol a file defines at file
#    scope -- function, struct, #define -- starts with pjv / PJV (or is the file's include guard).
#
# The scene's textures are the one exception to the uniform rule: engine-owned, but declared by the file
# that reads them (pjv_scene.sc, pjv_material.sc) rather than in pjv_engine.sc, because every pass
# includes the engine file and some bind their own inputs at the scene's stages.

# A script run with -P sets no policy version, and if(... IN_LIST ...) needs one.
cmake_minimum_required(VERSION 3.20)

set(sceneTextureFiles pjv_scene.sc pjv_material.sc)
set(sceneTextures tree64Data materialIDs materialPalette headerData gridInfo cellMap looseList
                  tree64Dims voxelTypeDims paletteDims)
set(profiles "spirv:linux" "440:linux" "metal:osx")

file(GLOB libraryFiles "${LIBRARY}/*.sc")
file(MAKE_DIRECTORY "${WORK}")
set(failures "")

# compile_test(name includes [defines]): a fragment shader defining `defines`, then including `includes`.
function(compile_test name includes)
    set(source "$input v_texcoord0\n")
    foreach(define IN LISTS ARGN)
        string(APPEND source "#define ${define}\n")
    endforeach()
    string(APPEND source "#include <bgfx_shader.sh>\n")
    foreach(include IN LISTS includes)
        string(APPEND source "#include <${include}>\n")
    endforeach()
    string(APPEND source "void main() { gl_FragColor = vec4(v_texcoord0, 0.0, 1.0); }\n")
    file(WRITE "${WORK}/${name}.frag" "${source}")
    foreach(entry IN LISTS profiles)
        string(REPLACE ":" ";" pair "${entry}")
        list(GET pair 0 profile)
        list(GET pair 1 platform)
        execute_process(
            COMMAND "${SHADERC}" -f "${WORK}/${name}.frag" -o "${WORK}/${name}.${profile}.bin"
                    --type f --platform ${platform} --profile ${profile} --varyingdef "${VARYING}"
                    -i "${BGFX_SHADER_INCLUDE}" -i "${LIBRARY}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
        if(NOT result EQUAL 0)
            set(failures "${failures}\n${name} (${profile}) does not compile:\n${output}" PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

set(allNames "")
foreach(file IN LISTS libraryFiles)
    get_filename_component(fileName "${file}" NAME)
    get_filename_component(stem "${file}" NAME_WE)
    list(APPEND allNames "${fileName}")
    compile_test("alone_${stem}" "${fileName}")

    file(STRINGS "${file}" lines)
    set(lineNumber 0)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")
        if(fileName STREQUAL "pjv_engine.sc")
            # The one file that declares uniforms; its own guard is the only symbol it defines.
        else()
            # One MATCHES per if(): CMake evaluates both sides of an OR, and the failing one clears
            # CMAKE_MATCH_1.
            set(declared "")
            if(line MATCHES "^[ \t]*uniform[ \t]+[A-Za-z0-9_]+[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
                set(declared "${CMAKE_MATCH_1}")
            elseif(line MATCHES "^[ \t]*[IU]?SAMPLER[A-Z0-9_]*\\([ \t]*([A-Za-z_][A-Za-z0-9_]*)")
                set(declared "${CMAKE_MATCH_1}")
            endif()
            if(declared AND NOT (fileName IN_LIST sceneTextureFiles AND declared IN_LIST sceneTextures))
                string(APPEND failures "\n${fileName}:${lineNumber}: declares a uniform; engine-owned ones go in pjv_engine.sc, a renderer's in the renderer")
            endif()
        endif()
        set(symbol "")
        if(line MATCHES "^#define[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
            set(symbol "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^struct[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
            set(symbol "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^[A-Za-z_][A-Za-z0-9_]*[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*\\(")
            set(symbol "${CMAKE_MATCH_1}")
        endif()
        string(TOUPPER "${stem}_SC" guard)
        if(symbol AND NOT symbol MATCHES "^(pjv|Pjv|PJV)" AND NOT symbol STREQUAL guard)
            string(APPEND failures "\n${fileName}:${lineNumber}: '${symbol}' has no pjv prefix")
        endif()
    endforeach()
endforeach()
compile_test("together" "${allNames}")
# The scene's feature gates (pjv_scene.sc) leave their code out by default, so the default compile says
# nothing about it: every gate on as well.
compile_test("scene_all_gates" "pjv_scene.sc"
             PJV_SCENE_TRANSPARENCY PJV_SCENE_REFRACTION PJV_SCENE_ANIMATION PJV_SCENE_MATERIAL PJV_SCENE_DEBUG)

if(failures)
    message(FATAL_ERROR "shader library:${failures}")
endif()
list(LENGTH libraryFiles count)
message(STATUS "shader library: ${count} files compile alone and together; the scan is clean")
