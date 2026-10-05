# Windows-only deployment: gathers the executable plus its Qt and MinGW runtime
# DLLs into build/dist, and adds the `installer` target that packs dist into an
# NSIS setup exe. Included from the top-level CMakeLists inside if(WIN32).

set(DIST_DIR "${CMAKE_BINARY_DIR}/dist")

get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${_qmake}" DIRECTORY)
find_program(WINDEPLOYQT_EXECUTABLE
    NAMES windeployqt6 windeployqt
    HINTS "${QT_BIN_DIR}"
    REQUIRED)

# The command-line tool ships beside the GUI: it needs a subset of the same Qt
# DLLs, which windeployqt stages for the GUI anyway.
if(MCU_STUDIO_BUILD_CLI)
    set(MCU_STUDIO_DEPLOY_CLI
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mcu-studio-cli> "${DIST_DIR}/")
    set(MCU_STUDIO_DEPLOY_DEPENDS mcu-studio mcu-studio-cli)
else()
    set(MCU_STUDIO_DEPLOY_CLI)
    set(MCU_STUDIO_DEPLOY_DEPENDS mcu-studio)
endif()

add_custom_target(deploy ALL
    DEPENDS ${MCU_STUDIO_DEPLOY_DEPENDS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:mcu-studio> "${DIST_DIR}/"
    ${MCU_STUDIO_DEPLOY_CLI}
    COMMAND "${WINDEPLOYQT_EXECUTABLE}"
        --no-translations --no-system-d3d-compiler --no-opengl-sw
        "${DIST_DIR}/mcu-studio.exe"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md" "${DIST_DIR}/"
    COMMENT "Deploying executable and Qt DLLs to dist/"
    VERBATIM)

# windeployqt only covers Qt itself; the MinGW runtime and third-party DLLs
# (libstdc++, libjpeg, ...) are resolved through ldd. MSYS2-only, which is the
# supported Windows build environment.
#
# The sweep must include the Qt plugins windeployqt staged under dist/, not just
# the exe: image-format plugins like imageformats/qjpeg.dll pull in
# libjpeg/libwebp/libtiff, which are reachable *only* through the plugin, never
# through the app exe. Missing those DLLs makes the plugin fail to load, and
# this app reads reference images in whatever format the system can decode, so
# a missing plugin dependency silently costs it formats. We scan every deployed
# exe/dll and loop to a fixpoint so a freshly copied dependency's own deps get
# pulled in too. Windows resolves a plugin's DLLs from the process exe's
# directory, so landing them all in dist/ is enough.
if(MINGW)
    add_custom_command(TARGET deploy POST_BUILD
        COMMAND bash -c "prev=-1; while true; do ldd $(find '${DIST_DIR}' -type f \\( -name '*.exe' -o -name '*.dll' \\)) 2>/dev/null | grep -oE '/(mingw64|ucrt64|clang64)/[^ ]+[.]dll' | sort -u | xargs -r cp -u -t '${DIST_DIR}'; n=$(find '${DIST_DIR}' -maxdepth 1 -name '*.dll' | wc -l); [ \"$n\" = \"$prev\" ] && break; prev=$n; done"
        COMMENT "Copying MinGW runtime + plugin DLLs to dist/"
        VERBATIM)
endif()

add_custom_target(installer
    COMMAND ${CMAKE_COMMAND}
        -DCMAKE_BINARY_DIR=${CMAKE_BINARY_DIR}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DAPP_VERSION=${PROJECT_VERSION}
        -P "${CMAKE_SOURCE_DIR}/installer/build_installer.cmake"
    DEPENDS deploy
    COMMENT "Building Windows installer with NSIS"
    VERBATIM)
