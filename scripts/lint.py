#!/usr/bin/env python3
# Static analysis on demand. Three tools, one table, one rule: a count may only
# shrink. The families that are already at zero do not live here — they are
# -Werror'd in the build itself (see the root CMakeLists); this script reports
# the debt that would be noise to compile in, so that it can be watched instead
# of ignored.
#
#   scripts/lint.py [build-dir]          report only
#   scripts/lint.py [build-dir] --gate   fail if any count moved
#
# The build dir must have a compile_commands.json, which the root CMakeLists now
# generates by default. Every tool reads its command line from that database
# instead of flags guessed at here — a hand-rolled cppcheck invocation once
# reported this tree as unanalyzable, which is how a tool that scans nothing
# looks identical to one that works.
import collections, concurrent.futures, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = next((a for a in sys.argv[1:] if not a.startswith('-')), os.path.join(ROOT, 'build'))
GATE = '--gate' in sys.argv
BASELINE = os.path.join(ROOT, 'scripts', 'lint-baseline.txt')

# fatal in the build, so they must be zero here too
GATED = {'Wuse-after-free', 'Wsign-compare'}
# reported, baseline-controlled
REPORT = ['-Wconversion', '-Wshadow', '-Wold-style-cast', '-Wsuggest-override']
DIAG = re.compile(r'^(/[^:]+):(\d+):\d+: (?:warning|error): .*?\[-?W?([a-z0-9-]+)\]$')
CLAZY = re.compile(r'^(/[^:]+):(\d+):\d+: warning: .* \[-Wclazy-([a-z0-9-]+)\]$')
CPPCHECK = re.compile(r'^(/[^:]+):(\d+):\d+: (?:style|warning|performance|portability): .* \[([a-zA-Z0-9_]+)\]$')
SKIP = ('/3rdparty/', '_autogen/', '/corrosion/')

def entries():
    db = os.path.join(BUILD, 'compile_commands.json')
    if not os.path.exists(db):
        sys.exit(f'no compile_commands.json in {BUILD} — configure that directory first')
    return [e for e in json.load(open(db)) if not any(s in e['file'] for s in SKIP)]

def run(cmd, cwd):
    p = subprocess.run(cmd, shell=isinstance(cmd, str), cwd=cwd, capture_output=True, text=True)
    return p.stdout + p.stderr

def syntax_pass(e):
    # the real command line, minus the object file and minus -Werror: this pass
    # is a report, and the build's own gate has already had its say
    cmd = re.sub(r'\s-o\s+\S+', ' -fsyntax-only', e['command'], count=1)
    cmd = re.sub(r'-Werror(=\S+)?', '', cmd)
    return run(cmd + ' ' + ' '.join(REPORT), e['directory']).splitlines()

def gcc_counts(srcs):
    found = collections.defaultdict(set)
    def one(e):
        for line in syntax_pass(e):
            m = DIAG.match(line)
            if m:
                found[m.group(3)].add((m.group(1), m.group(2)))
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        list(ex.map(one, srcs))
    return found

def clazy_counts(srcs):
    found = collections.defaultdict(set)
    def one(e):
        for line in run(['clazy-standalone', '-p', BUILD, e['file']], ROOT).splitlines():
            m = CLAZY.match(line)
            if m:
                found[m.group(3)].add((m.group(1), m.group(2)))
    # 4, not nproc: clazy is clang-tidy and each TU pays for the AST twice
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        list(ex.map(one, srcs))
    return found

def cppcheck_counts():
    found = collections.defaultdict(set)
    cmd = ['cppcheck', f'--project={os.path.join(BUILD, "compile_commands.json")}',
           '--library=qt', '--suppress=unknownMacro',
           '--enable=warning,performance,portability', '--inline-suppr', '--quiet']
    for line in run(cmd, ROOT).splitlines():
        m = CPPCHECK.match(line)
        if m and not any(s in m.group(1) for s in SKIP):
            found[m.group(3)].add((m.group(1), m.group(2)))
    return found

def show(name, found):
    print(f'\n{name}:')
    counts = {}
    for check, sites in sorted(found.items(), key=lambda kv: -len(kv[1])):
        # one header diagnostic repeats in every TU that includes it; what
        # matters is how many places in the tree still need fixing
        print(f'  {len(sites):6d}  {check}')
        counts[check] = len(sites)
    return counts

def main():
    srcs = entries()
    print(f'{len(srcs)} translation units, build dir {BUILD}')
    gcc = gcc_counts(srcs)
    gated = {k: v for k, v in gcc.items() if k in GATED}
    report_only = {k: v for k, v in gcc.items() if k not in GATED}
    now = {}
    now.update({('gcc', k): v for k, v in show('gcc, report-only families', report_only).items()})
    now.update({('clazy', k): v for k, v in show('clazy', clazy_counts(srcs)).items()})
    now.update({('cppcheck', k): v for k, v in show('cppcheck', cppcheck_counts()).items()})
    print('\ngcc, families the build fails on:')
    for check in sorted(GATED):
        print(f'  {gated.get(check, 0):6d}  {check}')
        now[('gcc-gated', check)] = gated.get(check, 0)

    base = {}
    if os.path.exists(BASELINE):
        for line in open(BASELINE):
            if line.strip() and not line.startswith('#'):
                tool, check, count = line.split()
                base[(tool, check)] = int(count)

    problems = []
    for key, n in sorted(now.items()):
        if key[0] == 'gcc-gated':
            if n:
                problems.append(f'GATED   {key[1]}: {n} — -Werror in the build, so this cannot be baselined')
            continue
        old = base.get(key)
        if n == 0 and old is None:
            continue
        if old is None:
            problems.append(f'NEW     {key[0]} {key[1]}: {n}')
        elif n > old:
            problems.append(f'GREW    {key[0]} {key[1]}: {old} -> {n}')
        elif n < old:
            problems.append(f'STALE   {key[0]} {key[1]}: {old} -> {n}, and a shrunk number is a deliberate edit')
        elif old == 0:
            problems.append(f'ZERO    {key[0]} {key[1]}: delete the line, it pins nothing')
    for key, old in sorted(base.items()):
        if key[0] != 'gcc-gated' and key not in now:
            problems.append(f'GONE    {key[0]} {key[1]}: baseline says {old}, tool reports nothing')

    if not GATE:
        print('\n(report only; --gate compares against scripts/lint-baseline.txt)')
        return
    if problems:
        print('\nbaseline differs:')
        for p in problems:
            print('  ', p)
        sys.exit(1)
    print('\nlint baseline holds')

main()
