# qimgv root
cmake_minimum_required(VERSION 3.13)
project(qimgv
    VERSION 1.0.2
    HOMEPAGE_URL "https://github.com/easymodo/qimgv"
    LANGUAGES CXX)

message(STATUS "Build configuration: " ${CMAKE_BUILD_TYPE})

# check for gcc
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
        CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    message(FATAL_ERROR "\n!!! THIS PROJECT REQUIRES GCC 9.0 OR LATER !!!\n")
endif()

# AUTOMOC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# USER OPTIONS
# Usage: cmake -DVIDEO_SUPPORT=ON [...]
option(EXIV2 "For reading/writing exif tags" ON)
option(VIDEO_SUPPORT "Enable video support" ON)
option(OPENCV_SUPPORT "Enable HQ scaling via OpenCV" ON)
option(KDE_SUPPORT "Support blur when using KDE" OFF)
option(USE_QT5 "Build with Qt5 if available" ON)

if(UNIX AND NOT APPLE)
    set(QT_EXTERN_PATH "" CACHE STRING "Tell compile external QT path, example: (/opt/Qt/6.2.0/gcc_64)")
    string(COMPARE EQUAL "${QT_EXTERN_PATH}" "" result)
    if (NOT result)
        set(CMAKE_INSTALL_RPATH "${QT_EXTERN_PATH}/lib")
    endif()
endif()

# FIND PACKAGES
if(USE_QT5) # Try Qt5 first if USE_QT5 is set
    find_package(Qt5 COMPONENTS Widgets QUIET)
    if(Qt5_FOUND)
        find_package(QT NAMES Qt5 REQUIRED COMPONENTS Core Widgets Svg PrintSupport)
    else()
        find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core Widgets Svg PrintSupport OpenGLWidgets)
    endif()
else() # Try Qt6 otherwise
    find_package(Qt6 COMPONENTS Widgets QUIET)
    if(Qt6_FOUND)
        find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core Widgets Svg PrintSupport OpenGLWidgets)
    else()
        find_package(QT NAMES Qt5 REQUIRED COMPONENTS Core Widgets Svg PrintSupport)
    endif()
endif()

message(STATUS "Qt Version: " ${QT_VERSION})
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Svg PrintSupport OpenGLWidgets)
else()
    find_package(Qt${QT_VERSION_MAJOR} 5.12 REQUIRED COMPONENTS Core Widgets Svg PrintSupport)
endif()

if(EXIV2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(Exiv2 REQUIRED IMPORTED_TARGET exiv2)
endif()

if(KDE_SUPPORT)
    find_package(KF6WindowSystem REQUIRED)
endif()

if(OPENCV_SUPPORT)
    find_package(PkgConfig REQUIRED)
    find_package(OpenCV REQUIRED core imgproc)
endif()

# --- Rust engine (corrosion: cargo integrated into this CMake build) --------
include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.2
)
FetchContent_MakeAvailable(Corrosion)
corrosion_import_crate(MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/engine/qimgv-engine-ffi/Cargo.toml)


##############################################################

# Must be at the top level: enable_testing() only generates a CTestTestfile for
# the directory it is called in and below, so calling it from qimgv/tests left
# `ctest` in the build root discovering zero tests.
enable_testing()

add_subdirectory(qimgv)

if(VIDEO_SUPPORT)
    add_subdirectory(plugins/player_mpv)
endif()

# Rust-side tests run from cargo, not cmake: corrosion only builds the cdylib/
# staticlib, so without this the engine's unit/integration tests are invisible
# to ctest and pass unnoticed when a developer forgets `cargo test`.
# Runs in the default workspace target dir (gitignored) so it reuses the warm
# cache an editor's `cargo test` already built — a private target dir would
# force a cold rebuild on every ctest run.
find_program(CARGO_EXECUTABLE cargo REQUIRED)
add_test(NAME ENGINE_RUST
         COMMAND ${CARGO_EXECUTABLE} test --workspace
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(ENGINE_RUST PROPERTIES TIMEOUT 600)
