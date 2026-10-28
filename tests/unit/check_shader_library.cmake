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
# pjv_utils_DDA.sc is exempt from the scan until S5 splits it: it declares the scene's samplers and
# predates the prefix. It is still compiled.

set(scanExempt pjv_utils_DDA.sc)
set(profiles "spirv:linux" "440:linux" "metal:osx")

file(GLOB libraryFiles "${LIBRARY}/*.sc")
file(MAKE_DIRECTORY "${WORK}")
set(failures "")

function(compile_test name includes)
    set(source "$input v_texcoord0\n#include <bgfx_shader.sh>\n")
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

    if(fileName IN_LIST scanExempt)
        continue()
    endif()
    file(STRINGS "${file}" lines)
    set(lineNumber 0)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")
        if(fileName STREQUAL "pjv_engine.sc")
            # The one file that declares uniforms; its own guard is the only symbol it defines.
        elseif(line MATCHES "^[ \t]*uniform[ \t]" OR line MATCHES "^[ \t]*[IU]?SAMPLER[A-Z0-9_]*\\(")
            string(APPEND failures "\n${fileName}:${lineNumber}: declares a uniform; engine-owned ones go in pjv_engine.sc, a renderer's in the renderer")
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

if(failures)
    message(FATAL_ERROR "shader library:${failures}")
endif()
list(LENGTH libraryFiles count)
message(STATUS "shader library: ${count} files compile alone and together; the scan is clean")
