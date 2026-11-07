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
helpers, mechanically enforced (R7). The former local exception
`forceInsertFileEntry` was retired 2026-11 — membership is the listing's
decision alone, no mime-sniff rescue for extension-less files. Architecture
rules are enforced by
`qimgv/tests/architecture_rules.py` (R1 gui isolation, R2 FFI speaker
whitelist, R3 rust seal, R4 settings deref, R5 ABI pin, R6 signal
redeclaration, R7 mirror-writer monopoly) — **structural rules live in the test suites, not in prose**.

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

1. **Start every bash command with `cd /home/dropguard/try/qimgv`.** cwd
   persists across calls; one command in build/ shatters the next. Failed a
   dozen times in a single day.
2. **Commits land on rustcore only**; feature work branches and merges back.
   Push only when the user says so.
3. **A fix and the test pinning it ship in the same commit.** Detection delay
   is the tuition.
4. **No implicit context**: cwd dependencies, singleton ordering, uninitialized
   members, protocols that only work in a call order — each already cost one
   fix (CacheItem/settings/lint.py).
5. **Signature-class fixes carry the function body.** f8e8b216 renamed
   signatures, claimed "no behavior change", shipped six regressions. When in
   doubt, read the full `git show` diff line by line; style families breed each
   other (fixing shadow created unused-parameter). Anything behavior-adjacent:
   pin it per rule 3.
6. **Fix only paths that compile; the only fact platform is Linux.** The
   engine watcher is inotify (`cfg(target_os = "linux")`), the event channel is
   `never()` elsewhere, the C++ fallback watcher was deleted — other platforms
   do not build. README Windows/BSD sections are marked upstream text, and
   `supportsWatching()` has no callers; an agent once "fixed a missing import"
   in a `cfg(windows)` branch unreachable at crate level. To touch another
   platform, get facts first: `rustup target add x86_64-pc-windows-gnu` +
   `cargo check --target ...` (check compiles; only runtime stays unverifiable).
7. **State locks never span waits.** Guards live to block end, not semicolon;
   sleeping under a read lock starves the engine writer (std RwLock has no
   writer preference) — b583e1aa's acceptance test failed exactly this way.
   Reference pattern: `wait_for` in the same file.
8. **Statistical measurement is a diagnostic bridge, never the deliverable.**
   N-run batches answer "how often", not "why"; close them into a proptest
   seed, an injected repro (applyArena/arenaFor was split for this), or a
   structural rule. Failures must leave a name — an unattributed 1/200 is
   unmeasured.

## Known debt (baselined, shrink-only)

- `scripts/lint-baseline.txt`: static-analysis counts, `--gate` enforces
  shrink-only. suggest-override / old-style-cast / shadow: zeroed;
  unused-parameter promoted to `-Werror`. No active style sweeps — new-family
  detection plus shrink-only does the policing.

## Standing decisions (change them and say why)

Decision history lives in commit messages — R7 rejection (319bb606), ownership
campaign / R4 (ee06b1e2), passedByValue (6bfbc58f), gen-thumbs (e91f4653), push
incident + gitignore vaccine (814f040b), mirror read path (3488601b). What is
kept here is only what past tense cannot hold: live states and reopen triggers.

- #16 (residual mirror/engine divergence): under observation since the
  structural fix — one unattributed failure in 200 rounds, one sighting with
  signature = engine holding deleted files + mirror missing created ones (each
  side drops a different event). QIMGV_ENGINE_TRACE=1 with timestamps armed;
  parked until the zero-sighting window is long enough.
- Sanitizer open items: ~58 KB object-graph leak at test exit (fixture cycles
  suspected); TSan tree not built (Rust needs -Zsanitizer, can't share a tree
  with ASan).
- Arena-as-storage parked; reopen trigger: million-entry measurement or a
  profile landing in parseListing.
- with_state closure is the real fix for discipline 7; reopen trigger: the next
  change to the FFI/test surface.
