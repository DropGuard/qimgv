# qimgv root
cmake_minimum_required(VERSION 3.16)  # Qt 6.6 requires CMake 3.16
project(impulse
    VERSION 0.1.0
    HOMEPAGE_URL "https://github.com/DropGuard/ImPulse"
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
# Qt6 only, and 6.9 is the floor: the fork has long stopped being buildable
# against Qt5 (the test suites hard-required Qt6, and OpenGLWidgets is linked
# unconditionally), so the selection dance only made both builds half-broken.
find_package(Qt6 6.9 REQUIRED COMPONENTS Core Widgets Svg PrintSupport OpenGLWidgets)
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

# Sanitizers: opt-in diagnostics trees, never the default build (2x+ runtime,
# and LSan sees Qt's exit-time allocations). Use a separate build directory:
#   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DQIMGV_SANITIZERS=address,undefined
# ASan+UBSan are the workhorses: heap misuse, leaks and UB are the failure
# classes a Rust-core/C++-shell app actually carries — the Rust side is
# memory-safe by construction, so what the sanitizers watch is exactly the
# C++ shell and the FFI boundary. Green here is a decidable property of the
# exercised paths, not a heuristic. TSan is its own tree — it cannot combine with
# ASan:
#   cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQIMGV_SANITIZERS=thread
# The engine stays uninstrumented there by measurement, not taste: instrumenting it
# takes nightly + -Zbuild-std (rustc refuses -Zsanitizer against a prebuilt std),
# and the mixed binary then reports nothing and dies at the same libtsan check the
# uninstrumented one dies at — no coverage bought, run lost.
# What the C++-only tree answers: C++/C++ sharing (Cache, the shared WorkerPool
# against the UI thread) is clean. What it cannot answer: the Rust side shows up
# only through memcpy interception, so the arena/listing reports name both
# accesses and see neither one's ordering — noise, not a queue of bugs; that
# noise is machine-absorbed by qimgv/tests/tsan-noise.supp, and a NEW report
# shape (any frame of ours) still fails the run.
#
# The registry CHECK that used to kill both long suites
# (sanitizer_thread_registry.cpp:186, live_.try_emplace duplicate-user_id guard)
# is fixed in-tree, not waited out. Root cause, pinned 2026-11-06: every join Qt
# 6.10 performs goes through pthread_clockjoin_np — the one join call TSan does
# not intercept (join/tryjoin_np/timedjoin_np are intercepted; clockjoin is
# absent from gcc-15's libtsan and clang-21's compiler-rt alike) — so the
# registry keeps each clockjoin'd pthread_t in its live_ map, glibc hands the
# same value back on the next pthread_create, and the CHECK aborts the process.
# Three churn sources fed it: QThreadPool's waitForDone() (tears the pool down
# and re-joins it per wave — repro, mechanism, lifecycle log and the erratum
# that its "40 waves" argument is dead live in ~/tsan-qthreadpool-check/;
# Qt 6.10's waitForDone ends in reset(), which joins and deletes every pool
# thread); QTest's WatchDog QThread, one per test class, joined at class end;
# and two are gone — the components run on the persistent WorkerPool (workers
# created once, joined once; the waves pin lives in workerpool_tests.cpp), and
# engine_stack_main runs one process per QtTest class so every watchdog gets a
# private registry. A fourth source is inside QtGui itself: the image paths
# spawn and clockjoin-join their own 'Thread (pooled)' threads (the same ones
# behind the QArrayData races below), and no in-tree code can intercept that —
# CORE_NAV, which decodes images and then creates threads, hit it once after
# the fixes above were declared final. core_nav_tests now forks per test
# FUNCTION: every function gets a private registry, measured 3/3 runs zero
# CHECKs. The CHECK is therefore not "eliminated" — it is made unreachable by
# construction wherever we own the process layout, and its one remaining
# in-process source belongs to QtGui.
# What still keeps the tree red is enumerated, and the enumeration is what a
# reader should trust instead of re-deriving it: the QArrayData-region races
# between QtGui's own 'Thread (pooled)' threads around image save/read —
# reproduced without any qimgv code in ~/tsan-qthreadpool-check/qtimagepool.cpp
# — plus stripped libQt6Gui/libtbb offsets that cannot be name-matched, plus
# the exit-time OpenBLAS teardown race (blas_thread_shutdown_ frees thread
# buffers while Core::~Core frees the window tree; libopenblas arrives through
# OpenCV); blanket-called_from_lib would hide every
# real app race through Qt, so the residue stays visible and the tree stays
# out of the gate until upstream grows a clockjoin interceptor (filed: LLVM
# #239326) or Qt's image paths stop racing themselves.
# Measured 2026-11-07, and verified present at 0a9559ea (the session-start
# commit, same three suites red — none of it introduced by the cancel /
# frame-name / shared-thumbnailer arcs): ENGINE_STACK and CORE_NAV stay red on
# queued-connection argument copies between two of OUR OWN WorkerPool threads
# (the memmove inside emit finished / taskStart delivery into
# LoaderRunnable::run and Thumbnailer::onTaskStart) landing on QArrayData
# blocks, QArrayData::reallocateUnaligned, and cleanOrphanedConnectionsImpl
# during widget teardown. The racing access is inside Qt's own container and
# connection machinery; our frames are only the callers.
# DECIDED 2026-11-07, this is the triage the residue block above awaited: the
# emit-path class reproduces with zero qimgv code — four std::threads, four
# sender QObjects, one UI-thread receiver, queued signals carrying a QString
# and a heap metatype, no event loop (~/tsan-qthreadpool-check/qsignals.cpp).
# 5/5 runs, same signature, and libQt6Core+0x29c364 — the exact offset the
# tree's reports name. Same verdict as the qtimagepool shape: Qt 6.10.2's own
# signal-argument path is a TSan race with no application fault involved. The
# residue is therefore upstream's, decided rather than deferred — but it is
# deliberately NOT suppressed: name-matching QArrayData::reallocateUnaligned
# or the arg-copy memmove would blind every real app race that flows through
# the same machinery, the exact over-reach the baseline's rules forbid. The
# tree stays out of the gate with the enumeration as its record, and any NEW
# report shape (a frame of ours beyond the emit callers, or a fresh third-party
# offset) still fails the run. The engine-boundary face of the noise is
# suppressed for real — a set ordering bug left ENGINE_CONTRACT's suppression
# property expanding to an empty env and 32 reports a run that looked like
# residue and were just a missing baseline (qimgv/tests/CMakeLists.txt,
# TSAN_ENV).
# The Rust half is measured by Miri over the semantics layer, on the seam the
# watcher split made explicit (watcher/apply is libc-free semantics; watcher/fd
# is real syscalls by design):
#   cd engine/qimgv-engine
#   PROPTEST_CASES=8 MIRIFLAGS="-Zmiri-disable-isolation" RUSTUP_TOOLCHAIN=nightly \
#   cargo +nightly miri test --lib --test watcher_tests --test model_tests \
#     --test property_tests --test meta_tests
# (prop cases reduced for interpreter overhead — ~6 min total, meta_tests alone
# ~4; -Zmiri-disable-isolation because the fixtures exercise real fs semantics;
# miri's own flags go through MIRIFLAGS, not the rustc command line.)
# Excluded, not skipped quietly: engine_tests and thumb_tests drive the real fd
# machinery (inotify FFI), which an interpreter refuses by design. The arming
# test is tests/race_arm.rs, #[ignore]d because its expected result IS the
# report:
#   cargo +nightly miri test --test race_arm -- --ignored
# → "Data race detected" at race_arm.rs:30; the nonzero exit IS the pass.
# The fd machinery's Rust side has no dynamic instrument of any kind — the
# standing named gap, accepted by owner's decision 2026-11-06 along with the
# retirement of the standalone -Zsanitizer=thread run that once covered it
# (nightly + -Zbuild-std, manual, never a gate; the pure layer's Miri check is
# wired as the opt-in MIRI ctest entry, -DMIRI_TESTS=ON, scripts/miri.sh).
# What bounds the gap is
# construction, not instrumentation: safe Rust cannot data-race, so the surface
# is the six unsafe syscall wrappers in watcher/fd.rs (none shares a pointer
# across threads) plus std/crossbeam internals, logic errors are not TSan's
# target anyway, and the boundary itself is watched by ASan and governed by the
# sole-writer contract. The re-measured MIXED setup below was the other
# candidate and is a measured dead end:
# Re-measured 2026-11-06, after the in-tree CHECK fixes unlocked it: the MIXED
# setup (this crate built with -Zsanitizer=thread -Zbuild-std via corrosion
# alongside the C++ tree) builds and RUNS — and it is not a usable instrument,
# for a sharper reason than the old note recorded. One process gets TWO TSan
# runtimes: gcc's libtsan.so for the C++ side and rustc's statically linked
# compiler-rt for the Rust side, each with its own thread registry. Qt threads
# register in one, Rust threads in the other, each join consumes in whichever
# registry the joiner resolves to, so live_ entries go permanently stale across
# the divide and the duplicate-user_id CHECK fires on the next pthread_create —
# the old "mixed binary reports nothing and dies at the same CHECK" was this
# split brain, not a coverage measurement. What the mixed run DID establish:
# with both sides instrumented the engine-side memcpy noise vanishes entirely
# (consistent shadow), and on every path the suites exercise — real watcher
# threads, real inotify — the Rust side reports CLEAN. The untested
# configuration that would make it a real instrument is one SHARED runtime
# generation for both sides (clang -shared-libsanitizer against a rustc that
# can link a shared tsan, or pointing the C++ link at rustc's runtime archive);
# everything else is measured dead ends. A green run in either instrument is
# partial evidence — judge findings, not the absence of them.
set(QIMGV_SANITIZERS "" CACHE STRING "sanitizer set for a diagnostics tree: address,undefined | thread (empty = the default build)")
if(QIMGV_SANITIZERS)
    # the two diagnostics trees are mutually exclusive: TSan cannot combine with
    # ASan, and a configure asking for both would produce a binary that is neither
    if(QIMGV_SANITIZERS MATCHES "address" AND QIMGV_SANITIZERS MATCHES "thread")
        message(FATAL_ERROR "QIMGV_SANITIZERS='${QIMGV_SANITIZERS}': 'thread' cannot combine with 'address' — use two build directories")
    endif()
    # the diagnostics trees want RelWithDebInfo, not Debug: the sanitizer passes
    # rewrite every access, so -O0 doubles what -O1/-O2 fold away through
    # inlining, while -g is what keeps the report stacks readable. Debug is
    # correct and useless here — the suites go from seconds to minutes for no
    # extra detection.
    if(NOT CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo|Release")
        message(WARNING "QIMGV_SANITIZERS with CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}': RelWithDebInfo is the supported shape (-O1/-O2 for size and speed, -g for stacks)")
    endif()
endif()
# The flags ride an INTERFACE target instead of global add_compile_options: the
# injection becomes a visible dependency of exactly the targets that are
# instrumented, and nothing else in the directory inherits it by accident.
# PUBLIC on the two object libraries propagates the link options up to the
# executable and the test binaries that link them.
add_library(qimgv_sanitizers INTERFACE)
if(QIMGV_SANITIZERS)
    target_compile_options(qimgv_sanitizers INTERFACE -fno-omit-frame-pointer -fsanitize=${QIMGV_SANITIZERS})
    target_link_options(qimgv_sanitizers INTERFACE -fsanitize=${QIMGV_SANITIZERS})
endif()

corrosion_import_crate(MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/engine/qimgv-engine-ffi/Cargo.toml)

# The fast gate wires itself in: every configure sets the repo's hooksPath,
# so clone -> configure -> build is already commit-enforced. The setup step
# is the configure, not a line of prose — prose setup steps are exactly the
# implicit context discipline 4 bans (a forgotten `git config` reopens the
# hole the hook exists to close). Tarball builds (no .git) skip silently:
# there is nothing to enforce hooks on. Idempotent by nature; undo with
# `git config --unset core.hooksPath`.
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} config core.hooksPath scripts/hooks
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_QUIET ERROR_QUIET)
endif()


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

# Miri over the Rust pure layer, opt-in for the same reason LINT is: ~8 minutes
# of interpreter time does not belong in every commit. scripts/miri.sh runs the
# semantics layer clean AND requires the armed race test to fail with its
# expected report — a passing armed run is an instrument declared dead, and any
# other failure is an instrument broken; both are red here.
option(MIRI_TESTS "Add the Rust pure-layer Miri run to ctest (nightly + miri component required)" OFF)
if(MIRI_TESTS)
    add_test(NAME MIRI COMMAND sh ${CMAKE_SOURCE_DIR}/scripts/miri.sh)
    set_tests_properties(MIRI PROPERTIES TIMEOUT 1200)
endif()
