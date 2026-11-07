# ImPulse

A high-performance Qt6 image viewer for Linux — a ground-up modernization fork
of [qimgv](https://github.com/easymodo/qimgv). GPL-3.0 · v0.1.0

## What changed from upstream

- **A Rust engine owns the backend.** Directory watching and decode scheduling
  left the C++ UI thread entirely — the multithreading crash class and the
  directory-update races of the original watcher are structurally gone, not
  patched over.
- **Renames are atomic.** The engine pairs the kernel's inotify cookies
  natively: when another program renames a file, the thumbnail survives and
  the selection stays put — no lost focus, no reloaded rows.
- **Your configuration migrates itself.** First run carries qimgv's settings,
  shortcuts and theme into ImPulse's own namespace; upstream qimgv stays
  installed and untouched.
- **The interface is upstream's, unchanged.** Same browsing, folder view,
  thumbnail panel, editing, scripts and shortcuts — the feature tour lives in
  [upstream's README](https://github.com/easymodo/qimgv#readme).

## Install

Download the AppImage from the
[releases page](https://github.com/DropGuard/ImPulse/releases), make it
executable and run — Qt, the codecs and the engine are all inside; nothing to
install on your system.

To build from source instead, see [DEVELOPING.md](DEVELOPING.md).

## For developers and contributors

The platform contract (Linux only), the architecture behind the three points
above, the build instructions, the five test suites (ctest, ASan/UBSan, TSan,
Miri) and the static-analysis gate are documented in
[DEVELOPING.md](DEVELOPING.md).

## License

GPL-3.0, as upstream. ImPulse is a fork of
[qimgv](https://github.com/easymodo/qimgv) by easymodo and contributors; the
interface and the artwork come from there. See `LICENSE`.
