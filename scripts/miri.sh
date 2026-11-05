#!/bin/sh
# The Rust pure layer's Miri check, machine-absorbed. Two parts, both must hold:
#   1. the semantics layer (watcher/apply, model, meta, property) interprets clean;
#   2. the armed race test FAILS with the expected "Data race detected" report —
#      a passing armed run means the instrument is dead, and any other failure
#      mode means it is broken; both are red here.
# Excluded by design, not skipped quietly: engine_tests and thumb_tests drive
# the real fd machinery (inotify FFI) whole-file; watcher_tests' five
# engine-spawning tests carry #[cfg_attr(miri, ignore)] in place, so the skip is
# visible in the run output. Reach and the fd-machinery named gap:
# QIMGV_SANITIZERS block in the root CMakeLists.txt.
# Requires: rustup toolchain nightly with the miri component.
set -u
cd "$(dirname "$0")/../engine/qimgv-engine" || exit 2

if ! cargo +nightly miri --version >/dev/null 2>&1; then
    echo "MIRI: cargo +nightly miri not available — rustup component add miri --toolchain nightly"
    exit 2
fi

echo "== pure layer =="
PROPTEST_CASES=8 MIRIFLAGS="-Zmiri-disable-isolation" RUSTUP_TOOLCHAIN=nightly \
    cargo +nightly miri test --lib --test watcher_tests --test model_tests \
        --test property_tests --test meta_tests || {
    echo "MIRI: pure layer failed"
    exit 1
}

echo "== armed race test =="
out=$(MIRIFLAGS="-Zmiri-disable-isolation" RUSTUP_TOOLCHAIN=nightly \
    cargo +nightly miri test --test race_arm -- --ignored 2>&1) || true
printf '%s\n' "$out" | grep -E "Data race detected|test result" | tail -4
case "$out" in
    *"Data race detected"*)
        echo "MIRI: armed and reporting — pass"
        exit 0
        ;;
    *"test result: ok"*)
        echo "MIRI: armed test PASSED — the instrument is dead"
        exit 1
        ;;
    *)
        echo "MIRI: armed run failed without a race report"
        exit 1
        ;;
esac
