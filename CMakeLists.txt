cmake_minimum_required(VERSION 3.21)

project(mcu-studio
    VERSION 1.2.0
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

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Concurrent Network)
find_package(JPEG REQUIRED)

# AI Fill's local model runs through ONNX Runtime. Without it the program
# builds and works as before, and the local model is listed as unavailable
# with the reason.
set(MCU_STUDIO_ONNX "AUTO" CACHE STRING "Use ONNX Runtime for the local AI Fill model (AUTO, ON, OFF)")
set_property(CACHE MCU_STUDIO_ONNX PROPERTY STRINGS AUTO ON OFF)
# ONNXRUNTIME_ROOT points at an unpacked release from the ONNX Runtime
# project, whose own CMake package file does not match its layout; without it
# the system's package is looked for.
set(ONNXRUNTIME_ROOT "" CACHE PATH "An unpacked ONNX Runtime release to use instead of a system package")
if(NOT MCU_STUDIO_ONNX STREQUAL "OFF")
    if(ONNXRUNTIME_ROOT)
        find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
            PATHS "${ONNXRUNTIME_ROOT}/include" "${ONNXRUNTIME_ROOT}/include/onnxruntime"
            NO_DEFAULT_PATH)
        find_library(ONNXRUNTIME_LIBRARY onnxruntime
            PATHS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/lib64" NO_DEFAULT_PATH)
        if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
            add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
            set_target_properties(onnxruntime::onnxruntime PROPERTIES
                IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}")
            set(onnxruntime_FOUND TRUE)
        endif()
    else()
        find_package(onnxruntime QUIET)
    endif()
    if(MCU_STUDIO_ONNX STREQUAL "ON" AND NOT onnxruntime_FOUND)
        message(FATAL_ERROR "MCU_STUDIO_ONNX is ON but ONNX Runtime was not found.")
    endif()
endif()
if(onnxruntime_FOUND)
    message(STATUS "ONNX Runtime found; the local AI Fill model is enabled.")
else()
    message(STATUS "ONNX Runtime not used; the local AI Fill model is disabled.")
endif()

option(MCU_STUDIO_BUILD_TESTS "Build the unit tests" ON)
option(MCU_STUDIO_BUILD_CLI "Build the mcu-studio-cli command-line tool" ON)
option(MCU_STUDIO_FUZZ "Build libFuzzer harnesses (needs clang)" OFF)

# The repair core reads coefficient arrays through libjpeg's virtual array
# manager one row group at a time, which is correct for any libjpeg, but its
# preview decoder is written to match libjpeg-turbo's output sample for
# sample. IJG libjpeg works, with previews that can differ from the exported
# file by a level on some samples.
include(CheckSymbolExists)
set(CMAKE_REQUIRED_INCLUDES ${JPEG_INCLUDE_DIRS})
check_symbol_exists(LIBJPEG_TURBO_VERSION_NUMBER "stdio.h;jpeglib.h" MCU_STUDIO_HAVE_TURBO)
unset(CMAKE_REQUIRED_INCLUDES)
if(NOT MCU_STUDIO_HAVE_TURBO)
    message(WARNING "libjpeg-turbo not found; previews may differ slightly from exported files.")
endif()

qt_standard_project_setup()

# Fuzzing instruments everything, so it has to be set before any target.
if(MCU_STUDIO_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MCU_STUDIO_FUZZ needs clang (CC=clang CXX=clang++)")
    endif()
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
                        -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Vendored jpegrepair, built as a library rather than a command-line program.
# Sources and licenses live in third_party/jpegrepair; see THIRD-PARTY-NOTICES.md.
# ---------------------------------------------------------------------------

add_library(jpegrepair_core STATIC
    third_party/jpegrepair/jpegrepair_core.c
    third_party/jpegrepair/jr_render.c
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

# Everything that is not a widget: the document model, file formats, analysis.
# The GUI, the command-line tool and the tests all link it.
add_library(mcu_core STATIC
    src/AiFill.cpp
    src/AiFill.h
    src/AiFillCloud.cpp
    src/AiFillLama.cpp
    src/AutoColor.cpp
    src/AutoColor.h
    src/Analysis.cpp
    src/Analysis.h
    src/Bitstream.cpp
    src/Bitstream.h
    src/ColorMath.cpp
    src/ColorMath.h
    src/DonorHeader.cpp
    src/DonorHeader.h
    src/DonorSearch.cpp
    src/DonorSearch.h
    src/DctEdge.h
    src/Exif.cpp
    src/Exif.h
    src/ImageDocument.cpp
    src/ImageDocument.h
    src/JpegRepair.cpp
    src/JpegRepair.h
    src/JpegStructure.cpp
    src/JpegStructure.h
    src/ProjectFile.cpp
    src/ProjectFile.h
    src/ReferenceFill.cpp
    src/ReferenceFill.h
    src/ReferenceImage.cpp
    src/ReferenceImage.h
    src/Report.cpp
    src/Report.h)
target_include_directories(mcu_core PUBLIC src)
target_link_libraries(mcu_core PUBLIC Qt6::Gui Qt6::Concurrent Qt6::Network jpegrepair_core)
target_compile_definitions(mcu_core PUBLIC MCU_STUDIO_VERSION="${PROJECT_VERSION}")
if(onnxruntime_FOUND)
    target_link_libraries(mcu_core PRIVATE onnxruntime::onnxruntime)
    target_compile_definitions(mcu_core PRIVATE MCU_HAVE_ONNX)
endif()
set_target_properties(mcu_core PROPERTIES AUTOMOC OFF)
if(MSVC)
    target_compile_options(mcu_core PRIVATE /W3)
else()
    target_compile_options(mcu_core PRIVATE -Wall -Wextra)
endif()

qt_add_executable(mcu-studio
    src/main.cpp
    src/AiFillDialog.cpp
    src/AiFillDialog.h
    src/AnalysisDock.cpp
    src/AnalysisDock.h
    src/BatchDialog.cpp
    src/BatchDialog.h
    src/DonorHeaderDialog.cpp
    src/DonorHeaderDialog.h
    src/EmbeddedImagesDialog.cpp
    src/EmbeddedImagesDialog.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/PlatformStyle.h
    src/ReferenceColorDialog.cpp
    src/ReferenceColorDialog.h
    src/ReferenceFillDialog.cpp
    src/ReferenceFillDialog.h
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
    Qt6::Network
    mcu_core)
# The version (MCU_STUDIO_VERSION) comes from mcu_core: --version, the About
# box and the installer all read it, and CI checks the release tag against it.

if(MCU_STUDIO_BUILD_CLI)
    qt_add_executable(mcu-studio-cli src/cli/main.cpp)
    target_link_libraries(mcu-studio-cli PRIVATE mcu_core)
    set_target_properties(mcu-studio-cli PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
    install(TARGETS mcu-studio-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(MCU_STUDIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MCU_STUDIO_FUZZ)
    add_subdirectory(fuzz)
endif()

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
