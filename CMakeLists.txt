cmake_minimum_required(VERSION 3.21)

project(mcu-studio
    VERSION 1.0.0
    DESCRIPTION "Coefficient-domain JPEG repair with an MCU-level editor"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keeps the Windows `deploy` target's staging simple: one known exe location.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Dependencies (system-provided)
# ---------------------------------------------------------------------------

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Concurrent)
find_package(JPEG REQUIRED)

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# Vendored jpegrepair, built as a library rather than a command-line program.
# Sources and licenses live in third_party/jpegrepair; see THIRD-PARTY-NOTICES.md.
# ---------------------------------------------------------------------------

add_library(jpegrepair_core STATIC
    third_party/jpegrepair/jpegrepair_core.c
    third_party/jpegrepair/transupp.c)

target_include_directories(jpegrepair_core PUBLIC third_party/jpegrepair)
target_link_libraries(jpegrepair_core PUBLIC JPEG::JPEG)
set_target_properties(jpegrepair_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(jpegrepair_core PRIVATE /W3)
else()
    target_compile_options(jpegrepair_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

qt_add_executable(mcu-studio
    src/main.cpp
    src/AutoColor.cpp
    src/AutoColor.h
    src/ColorMath.cpp
    src/ColorMath.h
    src/DonorHeader.cpp
    src/DonorHeader.h
    src/DonorHeaderDialog.cpp
    src/DonorHeaderDialog.h
    src/ImageDocument.cpp
    src/ImageDocument.h
    src/JpegRepair.cpp
    src/JpegRepair.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/PlatformStyle.h
    src/ProjectFile.cpp
    src/ProjectFile.h
    src/ReferenceColorDialog.cpp
    src/ReferenceColorDialog.h
    src/ReferenceFill.cpp
    src/ReferenceFill.h
    src/ReferenceFillDialog.cpp
    src/ReferenceFillDialog.h
    src/ReferenceImage.cpp
    src/ReferenceImage.h
    src/McuGridItem.cpp
    src/McuGridItem.h
    src/McuGraphicsView.cpp
    src/McuGraphicsView.h)

# The Windows icon resource. Also the reason the .rc is compiled only there.
if(WIN32)
    target_sources(mcu-studio PRIVATE packaging/mcu-studio.rc)
endif()

# The window icon, embedded so it shows up even when the app runs uninstalled
# and no .desktop/theme icon is on the system. The .ico is reused rather than
# the .svg because Qt reads it with the qico plugin from qt6-base, whereas SVG
# icons would pull in Qt6::Svg purely for this.
qt_add_resources(mcu-studio "appicon"
    PREFIX "/"
    BASE packaging
    FILES packaging/mcu-studio.ico)

target_link_libraries(mcu-studio PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    jpegrepair_core)

# Single source of truth for the version: --version, the About box and the
# installer all read this, and CI checks the release tag against it.
target_compile_definitions(mcu-studio PRIVATE
    MCU_STUDIO_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(mcu-studio PRIVATE /W3)
else()
    target_compile_options(mcu-studio PRIVATE -Wall -Wextra)
endif()

set_target_properties(mcu-studio PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_BUNDLE_NAME "MCU Studio"
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.thegameratort.mcustudio)

# Windows: `deploy` gathers the exe + runtime DLLs into build/dist, `installer`
# packs dist into an NSIS setup exe (see .github/workflows/build.yml).
if(WIN32)
    include(installer/windows.cmake)
endif()

install(TARGETS mcu-studio
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE THIRD-PARTY-NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Desktop integration. The app calls setDesktopFileName("mcu-studio") at
# startup, which is what makes Wayland/X11 associate the window with this
# .desktop file (and so the theme icon below) without a StartupWMClass
# override -- keep the basename here in sync with that call.
install(FILES packaging/mcu-studio.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/mcu-studio.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
