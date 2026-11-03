#!/usr/bin/env python3
# Executable architecture rules — the C++ answer to Java's ArchUnit.
# Each rule pins a structural property of the codebase that is true today
# and must stay true. A violation fails the test, so architecture drift
# surfaces in the same ctest run as everything else.
#
# Every rule reports how many files it actually scanned: a rule whose filter
# matches nothing is indistinguishable from a passing rule, and that is exactly
# how R1 slept for its whole life (it filtered by file NAME, so no gui/ file was
# ever looked at).
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'qimgv')

def sources():
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames[:] = [d for d in dirnames if d not in ('3rdparty', 'res', 'translations')]
        for f in filenames:
            if f.endswith(('.cpp', '.h')):
                path = os.path.join(dirpath, f)
                yield path, os.path.relpath(path, SRC).replace(os.sep, '/')

def text_of(path):
    return open(path, encoding='utf-8', errors='replace').read()

failures = []
scanned = {}

# Rule 1: the widget tree never speaks to the engine. The gui layer talks to
# Core / DirectoryModel only; engine access lives behind the components layer.
# The filter is on the path, not the file name — gui/ files are named
# mainwindow.cpp, viewerwidget.cpp and friends.
# Matched by header, not by the substring "engine": QPaintEngine and friends are
# ordinary Qt includes and the loose version of this rule flagged them.
ENGINE_ENTRY_POINTS = re.compile(r'(?:^|/)(?:qimgv_engine|enginebridge)\.h$')
for path, rel in sources():
    if not rel.startswith('gui/'):
        continue
    scanned['R1'] = scanned.get('R1', 0) + 1
    for inc in re.findall(r'#include\s+[<"]([^">]+)[">]', text_of(path)):
        if ENGINE_ENTRY_POINTS.search(inc):
            failures.append(f"R1 gui must not touch the engine: {rel} includes {inc}")

# Rule 2: the FFI header has a fixed set of authorized speakers. Anything else
# including qimgv_engine.h must join this list consciously — that review is
# the point of the rule.
ALLOWED_FFI_SPEAKERS = {
    'main.cpp',
    'components/directorymanager/enginebridge.cpp',
    'components/directorymanager/directorymanager.cpp',
    'components/thumbnailer/thumbnailerrunnable.cpp',
}
for path, rel in sources():
    if rel.startswith('tests/'):
        continue # the tests audit the C ABI and must include its header
    scanned['R2'] = scanned.get('R2', 0) + 1
    if re.search(r'#include\s+[<"]qimgv_engine\.h[">]', text_of(path)):
        if rel not in ALLOWED_FFI_SPEAKERS:
            failures.append(f"R2 unauthorized FFI speaker: {rel} (add to ALLOWED_FFI_SPEAKERS only after review)")

# Rule 3: Rust internals are invisible to C++. The engine is reachable only
# through its C ABI header; no source file may reach into the crate sources.
for path, rel in sources():
    scanned['R3'] = scanned.get('R3', 0) + 1
    if re.search(r'#include\s+.*(qimgv-engine/src|engine/qimgv-engine/src)', text_of(path)):
        failures.append(f"R3 Rust internals leaked: {rel}")

# Rule 4: code that runs off the UI thread never dereferences the settings
# global. One shared QSettings instance touched from several threads is a data
# race, and nothing else here can see it: no compiler warning, no clang-tidy
# check, no clazy check — the read is a plain global inside a QThreadPool
# worker. The fix is to carry the value in the request, as ScalerRequest does
# with smoothUpscaling, which also pins it to the moment the request was made.
#
# OFFTHREAD membership is by call graph, not by file name: sourcecontainers/*
# are decoded on the loader pool. DEBT lists the files that still read the
# global; it may only shrink, and an entry that has been cleaned fails the rule
# so the list cannot rot into a fiction.
OFFTHREAD = {
    'components/loader/loaderrunnable.cpp',
    'components/scaler/scalerrunnable.cpp',
    'components/thumbnailer/thumbnailerrunnable.cpp',
    'sourcecontainers/documentinfo.cpp',
    'sourcecontainers/imagestatic.cpp',
    'sourcecontainers/video.cpp',
}
DEBT = {
    'components/thumbnailer/thumbnailerrunnable.cpp',
    'sourcecontainers/documentinfo.cpp',
    'sourcecontainers/imagestatic.cpp',
    'sourcecontainers/video.cpp',
}
for path, rel in sources():
    if rel not in OFFTHREAD:
        continue
    scanned['R4'] = scanned.get('R4', 0) + 1
    lines = text_of(path).splitlines()
    hits = [i + 1 for i, l in enumerate(lines) if re.search(r'\bsettings\s*->', l)]
    if hits and rel not in DEBT:
        failures.append(f"R4 off-thread settings deref: {rel}:{hits[0]} — carry the value "
                        f"in the request (see ScalerRequest::smoothUpscaling) or add to DEBT after review")
    if not hits and rel in DEBT:
        failures.append(f"R4 stale DEBT entry: {rel} no longer reads settings — drop it from DEBT")

if failures:
    print("architecture violations:")
    for f in failures:
        print("  ", f)
    sys.exit(1)
print("architecture rules OK — " + ", ".join(f"{k} ({scanned.get(k, 0)} files)" for k in sorted(scanned)))
