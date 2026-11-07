# Developing ImPulse

The contributor-facing companion to the README: the platform contract, the
architecture, the build, and the verification machinery. The operating
discipline (each rule mapped to the incident that earned it) lives in
[AGENTS.md](AGENTS.md); the structural rules themselves are executable —
`qimgv/tests/architecture_rules.py` runs as part of ctest.

## Platform: Linux only

The directory listing belongs to a Rust engine whose watcher is inotify. On other
platforms the event channel is `never()`, the old `QFileSystemWatcher` fallback is
gone, and `inotify`/`libc` are unconditional dependencies — so live folder updates,
and the build itself, are not expected to work elsewhere. Picking another platform
up starts with `cargo check --target x86_64-pc-windows-gnu`: it does not link, so it
runs here and reports how much is missing before anything is written.

Distro packages named `qimgv` install upstream, not this fork.

## Architecture: what changed underneath

**One writer for the listing.** The Rust engine (`engine/qimgv-engine`) owns the
directory listing and decode scheduling. The C++ side is a read-only mirror plus
Qt rendering: mutation events carry engine-side indices and watcher-statted data,
and C++ only splices. The monopoly is enforced by a test, not by convention.

**A rename is one event, end to end.** The engine pairs the kernel's inotify
cookie, the wire carries both subjects, and the mirror moves a row instead of
killing one and growing another — so the thumbnail survives, the selection stays
put, and the viewer keeps showing the file you were looking at when another
program renames it. Pairing is best-effort by design: a pair can straddle a read
boundary, so remove+add remains a shape consumers must tolerate.

**Settings never touch worker threads.** The decode environment is captured once
per request on the UI thread and travels with the work (`LoadPolicy`); pool
threads receive values instead of reaching for global configuration.

**Quality machinery, as tests.** Ten structural architecture rules (R1–R10) run as
a ctest suite: GUI isolation, the FFI speaker whitelist, the Rust seal, settings
dereferencing off the UI thread, the ABI layout pin, the signal re-declaration ban,
the mirror-writer monopoly, the settings-table bypass ban, the single Thumbnailer
owner, and the ban on decision logic inside the syscall shell. A static-analysis
gate counts findings per family and pins its own instrument: the scanned
`(file, command)` set plus the versions of the tools that judged it. A
re-configured build tree or a compiler upgrade therefore fails the gate by name
instead of moving the counts silently. Counts may only shrink, and a reduction
ships with the fix that earned it.

## Building

Requirements: CMake >= 3.16, a C++17 compiler, ninja, and a Rust toolchain —
[Corrosion](https://github.com/corrosion-rs/corrosion) is fetched by CMake and runs
cargo inside the same build.

### Qt ≥ 6.9 — two roads

How you configure depends on where your Qt comes from. Note that re-running
`cmake -S . -B build` **without** `CMAKE_PREFIX_PATH` re-configures against the
system Qt and will fail the version check if that is older than 6.9 — keep the
flag from whichever road you took.

**Your distro ships Qt ≥ 6.9** (Debian 14, Ubuntu 25.10, Arch, …):

```bash
sudo apt install build-essential cmake ninja-build libgl1-mesa-dev libxkbcommon-dev \
                 qt6-base-dev libopencv-dev libexiv2-dev libmpv-dev
cmake -S . -B build -G Ninja
```

**Otherwise** (Ubuntu 24.04 LTS ships 6.4, Debian 13 ships 6.8) — the road the CI
itself takes is [aqtinstall](https://github.com/miurahr/aqtinstall), which downloads
official Qt binaries into a prefix of your choosing:

```bash
pip install aqtinstall
aqt install-qt linux desktop 6.9.1 linux_gcc_64 -O /opt/qt
sudo apt install build-essential cmake ninja-build libgl1-mesa-dev libxkbcommon-dev \
                 libopencv-dev libexiv2-dev libmpv-dev
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/opt/qt/6.9.1/gcc_64
```

Either way, then:

```bash
cmake --build build -j$(nproc)
```

The binary is `build/qimgv/impulse`. For desktop integration (menu entry,
icons, portal registration) run `scripts/install-user.sh` after the build — it
installs into `$HOME/.local` and needs no root.

| Option | Default | Effect |
| --- | --- | --- |
| `EXIV2` | ON | Read/write EXIF tags (needs Exiv2) |
| `OPENCV_SUPPORT` | ON | High-quality scaling filters (needs OpenCV core, imgproc) |
| `VIDEO_SUPPORT` | ON | Video playback plugin (needs libmpv) |
| `KDE_SUPPORT` | OFF | Blur behind windows on KDE (needs KF6WindowSystem) |
| `QIMGV_SANITIZERS` | empty | `address,undefined` or `thread`; use a separate build dir |
| `LINT_TESTS` | OFF | Add the static-analysis sweep to ctest (needs Python 3) |
| `MIRI_TESTS` | OFF | Add the Rust pure-layer Miri run to ctest (needs nightly + miri) |

Warnings are contractual: the build runs `-Wall -Wextra -Wpedantic`, and zero is
the normal result.

## Running

```bash
impulse /path/to/image-or-directory
```

## Testing

Verify a build:

```bash
cd build && ctest                       # five suites, including the architecture rules
cd engine/qimgv-engine && cargo test    # the engine's own suites
```

Maintainer pre-push gate — the clazy/cppcheck/gcc counts are baselined against
a pinned toolchain, and on a foreign toolchain the gate fails by design (the
instrument fingerprint names the change):

```bash
python3 scripts/lint.py build --gate    # static analysis against its pinned baseline
python3 scripts/coverage.py             # line coverage, logic and GUI reported separately
```

Sanitizers live in their own trees so the default build stays untouched:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQIMGV_SANITIZERS=address,undefined
cmake --build build-asan -j$(nproc) && cd build-asan && ASAN_OPTIONS=detect_leaks=1 ctest
```

The thread-sanitizer tree uses the same option with `thread`. Its enumerated
third-party noise baseline is `qimgv/tests/tsan-noise.supp`, and a report naming an
in-tree frame is still a failure. The engine's pure layer is checked with Miri
(`scripts/miri.sh`). New tests are run by name before their suite is trusted: a
green suite is only evidence if the test you think ran actually ran.

## AppImage

`scripts/build-appimage.sh` builds the self-contained bundle in a ubuntu:22.04
container (the glibc floor the bundle inherits: 2.35 — it covers Debian 12+ and
Ubuntu 22.04+). Everything rides inside: Qt 6.9, OpenCV, exiv2, libmpv and the
engine. Requires docker; no FUSE needed (appimagetool runs in extract-and-run
mode). The result lands in the repository root as
`ImPulse-<version>-x86_64.AppImage`.
