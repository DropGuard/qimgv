# ImPulse

A fast Qt6 image viewer for Linux. Fork of [qimgv](https://github.com/easymodo/qimgv) · GPL-3.0 · v0.1.0

The interface is upstream's and is unchanged: the same browsing, folder view,
thumbnail panel, crop/rotate/resize, quick copy/move, scripts and shortcuts. What
this fork changes is underneath it — the backend architecture, the machinery that
keeps it honest, and a long tail of bug fixes. For the feature tour, screenshots,
control scheme and usage notes, read
[upstream's README](https://github.com/easymodo/qimgv#readme); repeating it here
would only let the two drift apart.

## Linux is the only platform

The directory listing belongs to a Rust engine whose watcher is inotify. On other
platforms the event channel is `never()`, the old `QFileSystemWatcher` fallback is
gone, and `inotify`/`libc` are unconditional dependencies — so live folder updates,
and the build itself, are not expected to work elsewhere. Picking another platform
up starts with `cargo check --target x86_64-pc-windows-gnu`: it does not link, so it
runs here and reports how much is missing before anything is written.

Distro packages named `qimgv` install upstream, not this fork. Build from source.

## What changed underneath

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

Requirements: CMake >= 3.16, a C++17 compiler, Qt6 >= 6.9 (Core, Widgets, Svg,
PrintSupport, OpenGLWidgets), and a Rust toolchain —
[Corrosion](https://github.com/corrosion-rs/corrosion) is fetched by CMake and runs
cargo inside the same build.

### Obtaining Qt ≥ 6.9

Distro archives mostly ship older Qt (Ubuntu 24.04: 6.4, Debian 13: 6.8). The
road the CI itself takes is [aqtinstall](https://github.com/miurahr/aqtinstall),
which downloads official Qt binaries into a prefix of your choosing:

```bash
pip install aqtinstall
aqt install-qt linux desktop 6.9.1 linux_gcc_64 -O /opt/qt
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/opt/qt/6.9.1/gcc_64
```

The other dev libraries are apt names on Ubuntu/Debian
(`libopencv-dev libexiv2-dev libmpv-dev`); each maps to a build option above,
and turning the option OFF removes the dependency.

```bash
cmake -S . -B build
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

```bash
cd build && ctest                       # five suites, including the architecture rules
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
in-tree frame is still a failure. The engine's pure layer is checked with Miri.
New tests are run by name before their suite is trusted: a green suite is only
evidence if the test you think ran actually ran.

## License and credits

GPL-3.0, as upstream. ImPulse is a fork of
[qimgv](https://github.com/easymodo/qimgv) by easymodo and contributors; the
application, its interface and its artwork come from there. See `LICENSE`.
