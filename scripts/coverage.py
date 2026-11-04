#!/usr/bin/env python3
# Line coverage over the logic layer, so "what is untested" is a number
# instead of a memory. gcov-based (gcc's own), no new dependencies.
#
#   python3 scripts/coverage.py
#
# Covers the two C++ test binaries (engine_stack, core_nav). The Rust side
# keeps its own cargo suite and is not part of this number. Line coverage is
# a priority-sorting tool, not a gate: a percentage never proves anything
# tested, it ranks where the next test goes.

import glob, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build-cov')
OUT = '/tmp/qimgv-gcov'

def sh(cmd, cwd=ROOT):
    return subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True)

def main():
    if not os.path.exists(os.path.join(BUILD, 'CMakeCache.txt')):
        r = sh(f'cmake -S {ROOT} -B {BUILD} -DCMAKE_CXX_FLAGS=--coverage '
               f'-DCMAKE_EXE_LINKER_FLAGS=--coverage')
        if r.returncode:
            sys.exit(r.stderr[-2000:])
    r = sh(f'cmake --build {BUILD} --target engine_stack_tests core_nav_tests '
           f'-j{os.cpu_count()}')
    if r.returncode:
        sys.exit(r.stdout[-2000:] + r.stderr[-2000:])

    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    for rel in ('qimgv/tests/engine_stack_tests', 'qimgv/tests/core_nav_tests'):
        subprocess.run([os.path.join(BUILD, rel)], cwd=BUILD, env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    os.makedirs(OUT, exist_ok=True)
    for f in glob.glob(os.path.join(OUT, '*.gcov')):
        os.remove(f)
    sh(f'find {BUILD} -name "*.gcda" -print0 | xargs -0 -n4 gcov -b', cwd=OUT)

    src_re = re.compile(r'^\s*[-=#%]+\s*0:Source:\s*(.+)$')
    stats = {}
    for f in glob.glob(os.path.join(OUT, '*.gcov')):
        src, cov, unc = None, 0, 0
        for line in open(f, encoding='utf-8', errors='replace'):
            if src is None and '0:Source:' in line:
                src = line.split('0:Source:', 1)[1].strip()
                continue
            parts = line.split(':', 2)
            if len(parts) < 3:
                continue
            tag = parts[0].strip()
            if tag == '-':
                continue
            if tag == '#####':
                unc += 1
            elif tag == '=====':
                continue
            else:
                try:
                    int(tag)
                except ValueError:
                    continue
                cov += 1
        if not src or cov + unc == 0:
            continue
        if not src.startswith(os.path.join(ROOT, 'qimgv', '')):
            continue
        if '/build-cov/' in src or src.endswith('.moc') or '3rdparty' in src:
            continue
        rel = src.replace(os.path.join(ROOT, 'qimgv', ''), '')
        stats.setdefault(rel, [0, 0])
        stats[rel][0] += cov
        stats[rel][1] += unc

    logic = {k: v for k, v in stats.items() if not k.startswith('gui/')}
    gui = {k: v for k, v in stats.items() if k.startswith('gui/')}
    lc, lu = sum(v[0] for v in logic.values()), sum(v[1] for v in logic.values())
    g_c, g_u = sum(v[0] for v in gui.values()), sum(v[1] for v in gui.values())
    if lc + lu:
        print(f"logic layer: {lc}/{lc+lu} lines = {100*lc/(lc+lu):.0f}% covered")
    print(f"gui layer:   {g_c}/{g_c+g_u} lines = {100*g_c/(g_c+g_u):.0f}% covered")
    print("\nlogic layer, most uncovered:")
    for k, (c, u) in sorted(logic.items(), key=lambda kv: -kv[1][1])[:15]:
        t = c + u
        print(f"  {u:5d}/{t:5d} uncovered ({100*c/t:3.0f}%)  {k}")
    print("\nlogic files at 100%:", sum(1 for c, u in logic.values() if u == 0),
          "/", len(logic))

if __name__ == '__main__':
    main()
