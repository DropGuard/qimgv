#!/usr/bin/env python3
# Line coverage over the logic layer, so "what is untested" is a number
# instead of a memory. gcov --json-format (gcc's own machine-readable output),
# no new dependencies.
#
#   python3 scripts/coverage.py
#
# Covers the two C++ test binaries (engine_stack, core_nav). The Rust side
# keeps its own cargo suite and is not part of this number. Line coverage is
# a priority-sorting tool, not a gate: a percentage never proves anything
# tested, it ranks where the next test goes.
#
# The parser reads the JSON format deliberately: the text report is a
# human-readable render whose tag fields this tool once scraped with three
# failed regexes (written against an imagined format, not a real file) before
# landing on a substring. JSON failures are loud — a missing key is a
# traceback, not a quietly wrong count.

import glob, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, 'build-cov')

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

    stats = {}
    das = sorted(sh(f'find {BUILD} -name "*.gcda"').stdout.split())
    if not das:
        sys.exit('no .gcda files — the coverage build produced no test runs?')
    for gcda in das:
        r = sh(f'gcov --json-format -t "{gcda}"')
        if r.returncode:
            sys.exit(f'gcov failed on {gcda}: {r.stderr[-500:]}')
        # loud structure check: a format this tool does not recognize must
        # stop it here, never produce a quietly wrong count
        try:
            blob = json.loads(r.stdout)
            files = blob['files']
        except (json.JSONDecodeError, KeyError) as e:
            sys.exit(f'unexpected gcov JSON shape from {gcda}: {e}')
        cwd_field = blob.get('current_working_directory', '')

        def absolutize(p):
            return p if os.path.isabs(p) else os.path.join(cwd_field, p)

        for entry in files:
            src = os.path.normpath(absolutize(entry['file']))
            if not src.startswith(os.path.join(ROOT, 'qimgv', '')):
                continue
            if '/build-cov/' in src or src.endswith('.moc') or '3rdparty' in src:
                continue
            rel = src.replace(os.path.join(ROOT, 'qimgv', ''), '')
            slot = stats.setdefault(rel, [0, 0])
            for line in entry['lines']:
                slot[0 if line['count'] > 0 else 1] += 1

    logic = {k: v for k, v in stats.items()
             if not k.startswith('gui/') and not k.startswith('tests/')}
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
