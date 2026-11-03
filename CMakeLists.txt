# qimgv root
cmake_minimum_required(VERSION 3.16)  # Qt 6.6 requires CMake 3.16
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

# C++ standard. Declaring cxx_std_17 as a compile *feature* only asks for "at
# least 17", which GCC's own default already satisfies, so CMake writes no -std=
# flag at all — and CXX_EXTENSIONS OFF quietly stops doing anything. The variable
# form pins -std=c++17 on the command line, where it is visible and enforceable.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

if(UNIX AND NOT APPLE)
    set(QT_EXTERN_PATH "" CACHE STRING "Tell compile external QT path, example: (/opt/Qt/6.2.0/gcc_64)")
    string(COMPARE EQUAL "${QT_EXTERN_PATH}" "" result)
    if (NOT result)
        set(CMAKE_INSTALL_RPATH "${QT_EXTERN_PATH}/lib")
    endif()
endif()

# FIND PACKAGES
# Qt6 only, and 6.6 is the floor: the fork has already stopped being buildable
# against Qt5 (the test suites hard-required Qt6, and OpenGLWidgets is linked
# unconditionally), so the selection dance only made both builds half-broken.
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets Svg PrintSupport OpenGLWidgets)
message(STATUS "Qt Version: " ${Qt6_VERSION})

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
# Tier-0 static analysis: the warnings are part of the contract. The build is
# expected to be warning-free; the audit tests and the settings/mirror tables
# rely on new code failing loudly here.
add_compile_options(-Wall -Wextra -Wpedantic)

# clang-tidy / clazy / cppcheck all read the command line rather than guess it,
# so the compilation database is part of the toolchain, not a convenience.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Families at zero findings in this tree are errors. They are the classes this
# project has actually been bitten by: a descriptor closed twice
# (use-after-free), an int compared against a size() (sign-compare), and a
# parameter rename that left six call sites reading members instead of their
# arguments (unused-parameter — the only diagnostic that pointed at f8e8b216's
# behavior breakage). The rest are deliberately NOT compiled in, because a
# build that shouts is a build nobody reads: the style/coordinate-math families
# are retired from the gate scan entirely (scripts/lint.py FROZEN and REPORT),
# and the remainder runs on demand against a baseline that may only shrink.
add_compile_options(-Werror=sign-compare)
add_compile_options(-Werror=unused-parameter)
# -Wuse-after-free is a GCC check; the clang driver that clazy wraps accepts the
# flag only by warning that it does not know it, so asking for it there would
# print two lines per TU and enforce nothing.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-Wuse-after-free -Werror=use-after-free)
endif()

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

# The static-analysis sweep is minutes, not seconds, so it is not in the default
# ctest run: -DLINT_TESTS=ON adds it for whoever wants the whole gate in one
# command. Otherwise scripts/lint.py stays the on-demand path, which is what the
# baseline rule assumes — the numbers move when a human says they may.
option(LINT_TESTS "Add the static-analysis sweep to ctest" OFF)
if(LINT_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME LINT
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/lint.py
                     --gate ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(LINT PROPERTIES LABELS lint TIMEOUT 3600)
endif()
