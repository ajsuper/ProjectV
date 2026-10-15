# Documentation targets. None of them is part of the default build.
#
#   cmake --build build --target docs        the web site, with the API reference if Doxygen is found
#   cmake --build build --target docs-serve  the same, served at http://127.0.0.1:8000 and rebuilt on save
#   ctest --preset dev -R docs               the link check (also part of the ordinary ctest run)
#
# The Markdown under docs/ needs none of this: it is written to be read with cat and grep.
# The site is MkDocs + Material, installed into a venv in the build directory on first use
# (tools/docs/requirements.txt pins the versions). See docs/contributing/writing-docs.md.

find_package(Python3 COMPONENTS Interpreter QUIET)
find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)

if(NOT Python3_Interpreter_FOUND)
    message(STATUS "ProjectV docs: no Python 3, so no docs targets and no docs test")
    return()
endif()

set(PROJV_DOCS_DIR  "${CMAKE_BINARY_DIR}/docs")
set(PROJV_DOCS_VENV "${PROJV_DOCS_DIR}/venv")
set(PROJV_DOCS_API  "${PROJV_DOCS_DIR}/api")
set(PROJV_DOCS_SITE "${PROJV_DOCS_DIR}/site")

add_custom_command(
    OUTPUT  "${PROJV_DOCS_VENV}/installed.stamp"
    COMMAND "${Python3_EXECUTABLE}" -m venv "${PROJV_DOCS_VENV}"
    COMMAND "${PROJV_DOCS_VENV}/bin/python" -m pip install --quiet -r "${PROJECT_SOURCE_DIR}/tools/docs/requirements.txt"
    COMMAND "${CMAKE_COMMAND}" -E touch "${PROJV_DOCS_VENV}/installed.stamp"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/docs/requirements.txt"
    COMMENT "Installing MkDocs into ${PROJV_DOCS_VENV}"
    VERBATIM)

set(_projv_api_commands)
if(DOXYGEN_FOUND)
    if(TARGET Doxygen::dot)
        set(_projv_dot YES)
    else()
        set(_projv_dot NO)
    endif()
    set(_projv_api_commands
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJV_DOCS_API}"
        COMMAND "${CMAKE_COMMAND}" -E env PROJV_DOXYGEN_OUT=${PROJV_DOCS_API} PROJV_DOXYGEN_DOT=${_projv_dot}
                "${DOXYGEN_EXECUTABLE}" tools/docs/Doxyfile)
else()
    message(STATUS "ProjectV docs: Doxygen not found; the site's API page will say how to build it")
endif()

# NO_MKDOCS_2_WARNING: Material prints a banner about MkDocs 2.0 on every build. The versions are
# pinned to MkDocs 1.6 (tools/docs/requirements.txt), which the banner does not affect.
set(_projv_mkdocs_env "${CMAKE_COMMAND}" -E env PROJV_API_HTML=${PROJV_DOCS_API}/html NO_MKDOCS_2_WARNING=true)

add_custom_target(docs
    ${_projv_api_commands}
    COMMAND ${_projv_mkdocs_env} "${PROJV_DOCS_VENV}/bin/mkdocs" build --strict --quiet
            --config-file mkdocs.yml --site-dir "${PROJV_DOCS_SITE}"
    COMMAND "${CMAKE_COMMAND}" -E echo "Docs: ${PROJV_DOCS_SITE}/index.html"
    DEPENDS "${PROJV_DOCS_VENV}/installed.stamp"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM)

add_custom_target(docs-serve
    ${_projv_api_commands}
    COMMAND ${_projv_mkdocs_env} "${PROJV_DOCS_VENV}/bin/mkdocs" serve --config-file mkdocs.yml
    DEPENDS "${PROJV_DOCS_VENV}/installed.stamp"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    USES_TERMINAL
    VERBATIM)

if(PROJV_BUILD_TESTS)
    add_test(NAME docs COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/docs/check_links.py")
endif()
