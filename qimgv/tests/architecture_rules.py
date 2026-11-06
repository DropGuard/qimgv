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
# race, and ambient reads cannot be pinned to the moment a request was made:
# the value travels in the request instead — ScalerRequest::smoothUpscaling,
# LoadPolicy for the decode/classification chain, explicit quality for saves.
# The DEBT list that used to live here retired at zero on 2026-11-05
# (thumbnailerrunnable, documentinfo, imagestatic, video); the rule is a pure
# assertion now.
OFFTHREAD = {
    'components/loader/loaderrunnable.cpp',
    'components/scaler/scalerrunnable.cpp',
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
    if hits:
        failures.append(f"R4 off-thread settings deref: {rel}:{hits[0]} — carry the "
                        f"value in the request (see ScalerRequest::smoothUpscaling / LoadPolicy)")

# Rule 5: both sides of the ABI assert the same layout. qimgv_engine.h pins the
# by-value structs with QIMGV_PIN, lib.rs mirrors the numbers with size_of/
# offset_of. This has to be a rule because neither compiler can see the other: a
# layout mismatch still links (the symbol names are unchanged), so the only thing
# that can catch a field reordered on one side is comparing the two sets of
# literals — and sizes alone would miss exactly that case, since reordering keeps
# the size identical.
HEADER = os.path.join(ROOT, 'engine/qimgv-engine-ffi/include/qimgv_engine.h')
RUST = os.path.join(ROOT, 'engine/qimgv-engine-ffi/src/lib.rs')
for side, path in (('header', HEADER), ('rust', RUST)):
    if not os.path.exists(path):
        failures.append(f"R5 cannot run: the {side} moved ({path}) — update this rule, do not let it scan nothing")
if not any(f.startswith('R5 cannot run') for f in failures):
    scanned['R5'] = 2

    def pascal_to_snake(name):
        return re.sub(r'(?<!^)(?=[A-Z])', '_', name).lower()

    def c_layout(text):
        out = {}
        for struct, n in re.findall(r'QIMGV_PIN\("(qimgv_[a-z_]+)", sizeof\(qimgv_[a-z_]+\) == (\d+)\)', text):
            out[(struct, '')] = int(n)
        for field, struct, n in re.findall(
                r'QIMGV_PIN\("qimgv_[a-z_]+\.([a-z_]+)", offsetof\((qimgv_[a-z_]+), \w+\) == (\d+)\)', text):
            out[(struct, field)] = int(n)
        return out

    def rust_layout(text):
        out = {}
        for struct, n in re.findall(r'size_of::<(Qimgv\w+)\>\(\) == (\d+)', text):
            out[('qimgv_' + pascal_to_snake(struct.removeprefix('Qimgv')), '')] = int(n)
        for struct, field, n in re.findall(r'offset_of!\((Qimgv\w+), (\w+)\) == (\d+)', text):
            out[('qimgv_' + pascal_to_snake(struct.removeprefix('Qimgv')), field)] = int(n)
        return out

    c, r = c_layout(text_of(HEADER)), rust_layout(text_of(RUST))
    for key in sorted(set(c) | set(r)):
        struct, field = key
        where = f"{struct}.{field}" if field else struct
        if key not in c:
            failures.append(f"R5 unpinned in the C header: {where}")
        elif key not in r:
            failures.append(f"R5 unpinned in Rust: {where}")
        elif c[key] != r[key]:
            failures.append(f"R5 ABI layout disagrees: {where} is {c[key]} in qimgv_engine.h, {r[key]} in lib.rs")

# Rule 6: no class re-declares a signal an ancestor already declares. Qt gives the
# derived class its OWN signal when the name is re-declared, so a consumer that
# connects through &Base::sig silently receives nothing — measured, not theorised:
# a base-pointer connection to a re-declared signal got 0 deliveries while the
# derived one got 1. That is how "video finished, advance" was dead: the proxy
# re-declared playbackFinished while ViewerWidget connects via
# &VideoPlayer::playbackFinished (viewerwidget.cpp:53).
BASE_RE = re.compile(r'^\s*class\s+(\w+)\b[^;{]*?:\s*((?:\w+\s*:\s*)?public\s+\w+(?:\s*,\s*public\s+\w+)*)')
SIG_RE = re.compile(r'^\s*(?:void|[A-Za-z_]\w*<[^>]*>)\s+(\w+)\s*\([^;]*\)\s*;')
MODE_END = re.compile(r'^\s*(public|protected|private|signals|Q_SIGNALS|slots)\b')
signals_of, parents = {}, {}
for root in (SRC, os.path.join(ROOT, 'plugins')):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ('3rdparty', 'res', 'translations')]
        for f in filenames:
            if not f.endswith('.h'):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
            scanned['R6'] = scanned.get('R6', 0) + 1
            current, in_signals = None, False
            for line in text_of(path).splitlines():
                m = BASE_RE.match(line)
                if m:
                    current = m.group(1)
                    signals_of.setdefault(current, set())
                    parents.setdefault(current, set())
                    parents[current] |= {b.strip().split()[-1] for b in m.group(2).split(',')}
                    in_signals = False
                    continue
                if not current:
                    continue
                e = MODE_END.match(line)
                if e:
                    in_signals = e.group(1) in ('signals', 'Q_SIGNALS')
                    continue
                if in_signals:
                    s = SIG_RE.match(line)
                    if s:
                        signals_of[current].add(s.group(1))

def ancestors(name, seen=()):
    for parent in parents.get(name, ()):
        if parent in seen:
            continue
        yield parent
        yield from ancestors(parent, seen + (parent,))

for name, sigs in sorted(signals_of.items()):
    for parent in sorted(set(ancestors(name))):
        for sig in sorted(sigs & signals_of.get(parent, set())):
            failures.append(f"R6 signal re-declared: {name}::{sig}() also exists in {parent} — "
                            f"connections made through &{parent}::{sig} will never fire")

# Rule 7: the mirror has exactly one writer family. fileEntryVec/dirEntryVec may
# only be mutated inside the engine-apply functions; every other local write is
# the forceInsert-shaped exception that retired this rule's subject. Deterministic
# single-file scan (function attribution by definition regex) — not the rejected
# line-level lock heuristic: no sampling, no green-means-sampled downgrade.
MIRROR_WRITERS = {'applyArena', 'removeFileEntry', 'removeDirEntry'}
MUT_RE = re.compile(r'(?:file|dir)EntryVec\.(push_back|insert|erase|clear|assign)')
FN_RE = re.compile(r'^[a-zA-Z].*\bDirectoryManager::(\w+)')
dm_path = os.path.join(SRC, 'components', 'directorymanager', 'directorymanager.cpp')
if os.path.exists(dm_path):
    scanned['R7'] = scanned.get('R7', 0) + 1
    fn = '?'
    for lineno, line in enumerate(text_of(dm_path).splitlines(), 1):
        m = FN_RE.match(line)
        if m:
            fn = m.group(1)
        if MUT_RE.search(line) and fn not in MIRROR_WRITERS:
            failures.append(f"R7 mirror writer: directorymanager.cpp:{lineno} mutates the "
                            f"listing inside {fn}() — only {sorted(MIRROR_WRITERS)} may")
else:
    failures.append("R7 cannot run: directorymanager.cpp missing — scanned 0 files is not a pass")

# Rule 8: the settings table is the only description of a setting's bounds.
# A raw QSettings read of a key the table declares bypasses the row's junk and
# range policy — which is how JPEGSaveQuality ended up saving at quality 0 on a
# corrupt config (QSettings applies its second argument only to a MISSING key,
# junk converts to 0, and 0 was inside the declared range). Keys whose accessor
# maps the stored value to something the row cannot express are listed here
# deliberately, the way R2 lists authorized FFI speakers: adding a name to this
# set is the review, and the review has to be read.
TABLE_KEYS = set()
table_path = os.path.join(SRC, 'settings_table.h')
if os.path.exists(table_path):
    TABLE_KEYS = set(re.findall(r'=\s*(?:INT|INT_RESET|REAL|BOOL|STR)\("([^"]+)"', text_of(table_path)))
RAW_BYPASS = re.compile(r'(?:settingsConf|stateConf|themeConf)->(?:value|setValue)\(\s*"([^"]+)"')
MAPPING_EXEMPT = {
    'mpvBinary',      # resolves to a fallback binary by file existence
    'panelPosition',  # maps a string to an enum member, else-branch is the default
}
settings_path = os.path.join(SRC, 'settings.cpp')
if not TABLE_KEYS:
    failures.append("R8 cannot run: settings_table.h keys parsed to zero — update this rule, do not let it scan nothing")
elif os.path.exists(settings_path):
    scanned['R8'] = scanned.get('R8', 0) + 1
    for lineno, line in enumerate(text_of(settings_path).splitlines(), 1):
        for key in RAW_BYPASS.findall(line):
            if key in TABLE_KEYS and key not in MAPPING_EXEMPT:
                failures.append(f"R8 table key read raw: settings.cpp:{lineno} uses \"{key}\" "
                                "through QSettings directly — go through load/store and its row")

if failures:
    print("architecture violations:")
    for f in failures:
        print("  ", f)
    sys.exit(1)
print("architecture rules OK — " + ", ".join(f"{k} ({scanned.get(k, 0)} files)" for k in sorted(scanned)))
