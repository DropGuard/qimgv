# qimgv fork — agent operating manual

Why this file exists: every rule here maps to a real incident, and memory is
the worst place to store rules. Read it before editing it.

## What this repo is

A qimgv fork (KDE/Qt6-first, GPL). `rustcore` is the integration trunk: the
Rust engine (`engine/qimgv-engine`) is the sole writer of directory and
decode scheduling; the C++ side is a read-only mirror plus Qt rendering.
Mirror content and order are decided by the engine — mutation events carry
engine-side indices and watcher-statted data, C++ only splices (contract:
`mirrorStaysAFaithfulProjectionOfTheEngineListing`), and the contract is
absolute: the only listing writers are `applyArena` and the two event-apply
helpers, mechanically enforced (R7). Architecture rules are enforced by
`qimgv/tests/architecture_rules.py` (R1 gui isolation, R2 FFI speaker
whitelist, R3 rust seal, R4 settings deref, R5 ABI pin, R6 signal
redeclaration, R7 mirror-writer monopoly) — **structural rules live in the
test suites, not in prose**. Two ownership rules are absolute and are
enforced by construction rather than by discipline: the QObject parent tree
owns every widget and every other pointer only observes (the generalized
`mainpanel.cpp` rule), and the WorkerPool owns every job (submit transfers,
drain returns, the pool destroys completed jobs on its worker thread;
workers are created once and joined once — nothing else may create or join a
pipeline thread).

## Everyday commands

```bash
# build (warnings are contractual: -Wall -Wextra -Wpedantic, zero is normal)
cmake --build build -j$(nproc)

# all tests (5 suites; ARCH_RULES is the architecture contract — read its output first on failure)
cd build && ctest

# static-analysis gate (counts may only shrink; a new family fails; a reduction ships with its fix)
python3 scripts/lint.py build --gate

# clazy, separate configured tree (clazy 1.17 + level0/level1/detaching)
CLAZY_CHECKS="level0,level1,detaching" cmake --build build-clazy -j$(nproc)

# line coverage (logic/GUI layers reported separately; ranks test work only, not a gate)
python3 scripts/coverage.py

# dev tripwire: any qWarning aborts with a stack. Silent-rot bugs (the Qt6-dead
# @Variant serialization was a live one) surface on first contact. Startup
# warnings now reach the session log (handler installs before any config read).
QT_FATAL_WARNINGS=1 build/qimgv/qimgv <path>

# sanitizer tree (separate dir; default build untouched). Note: suites can go red
# on LSan exit leaks even with all asserts passing — evidence is in
# Testing/Temporary/LastTest.log, not ctest output.
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQIMGV_SANITIZERS=address,undefined
cmake --build build-asan -j$(nproc) && cd build-asan && ASAN_OPTIONS=detect_leaks=1 ctest

# the thread-sanitizer tree is the same option with `thread`. Its state is
# documented at the option itself (QIMGV_SANITIZERS block of CMakeLists.txt)
# and its noise baseline lives in qimgv/tests/tsan-noise.supp: out of the gate
# on enumerated third-party internals (QtGui's own pooled threads racing the
# QArrayData region around image save/read), with any report naming one of our
# frames still a failure. The harness forks per class / per test function so
# every QTest watchdog gets a private TSan registry. The Rust pure layer is
# Miri-checked — reach, command and the fd-machinery named gap are in the
# same block.
```

The pre-commit hook runs incremental build + 5 suites (~10-30 s) and is
**CMake self-installing**: every configure points `core.hooksPath` at
`scripts/hooks` — clone → configure → build is itself the install step.
`lint --gate` takes ~2m20s; run it manually before pushing. The `--no-verify`
escape hatch exists, but a red gate entering history is how f8e8b216's six
silent regressions went unseen.

## Discipline (each maps to an incident)

Whatever a machine can absorb, a machine absorbed (hooks, arch rules, gates,
baselines). What remains here is the part needing judgment.

1. **Start every bash command by cd-ing to the repo root via its absolute
   path.** cwd persists across calls; one command left in build/ shatters the
   next. Failed a dozen times in a single day.
2. **A fix and the test pinning it ship in the same commit; behavior-adjacent
   refactors must prove their innocence.** Detection delay is the tuition.
   Signature-only sweeps are not automatically behavior-preserving — f8e8b216
   renamed signatures, claimed "no behavior change", shipped six regressions.
   Read the full `git show` diff line by line in the function body; anything
   that could change behavior gets pinned.
3. **No implicit context**: cwd dependencies, singleton ordering, uninitialized
   members, protocols that only work in a call order — each already cost one
   fix.
4. **Fix only paths that compile; the only fact platform is Linux.** The
   engine watcher is inotify, the event channel is `never()` elsewhere, the
   C++ fallback watcher was deleted — other platforms do not build. An agent
   once "fixed a missing import" in a `cfg(windows)` branch unreachable at
   crate level. To touch another platform, get facts first: `rustup target add
   x86_64-pc-windows-gnu` + `cargo check --target ...` (check compiles; only
   runtime stays unverifiable).
5. **State locks never span waits.** Guards live to block end, not semicolon;
   sleeping under a read lock starves the engine writer (std RwLock has no
   writer preference). Reference pattern: `wait_for` in the same file.
6. **Statistical measurement is a diagnostic bridge, never the deliverable.**
   N-run batches answer "how often", not "why"; close them into a proptest
   seed, an injected repro, or a structural rule. Failures must leave a name —
   an unattributed 1/200 is unmeasured.
7. **A green suite is only evidence if the test you think ran actually ran.**
   QTest cannot see test functions declared below `private:`; a filter typo
   runs nothing. The ownership probe (3e4c2f37) passed five suites while
   itself never executing — caught by running the new test by name and seeing
   "Function not found". Before trusting a suite, run the new test by name
   and watch it PASS.
8. **One clean instrument run is a sample, not a property.** "Zero CHECKs"
   was declared off a single run and the fourth clockjoin source — QtGui's
   own pooled threads, joined inside QtGui where no in-tree code can
   intercept — surfaced on the next. When a fix claims an invariant, re-run
   the instrument that would catch its violation, several times for
   timing-dependent triggers, or make the violation structurally impossible
   (the fork-per-test harness exists for exactly that).
