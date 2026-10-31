#!/usr/bin/env python3
# end to end: every query of queries.sql through sql compile -> opt plan -> exec run, at several frame
# budgets, optimised and unoptimised, and through the db shell, against PostgreSQL on the same data. Usage:
#   python3 tests/e2e/run.py SCALE "psql connection arguments"
import json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORK = os.path.join(ROOT, 'tests', 'tmp-e2e')
scale, psql = sys.argv[1], sys.argv[2].split()

def run(cmd, stdin=None):
    r = subprocess.run(cmd, capture_output=True, text=True, input=stdin)
    if r.returncode != 0: raise RuntimeError(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r.stdout

def tool(name, *args): return run([os.path.join(ROOT, name), *args])

def statements(path):
    text = ''.join(l for l in open(path) if not l.lstrip().startswith('--'))
    return [s.strip() + ';' for s in text.split(';') if s.strip()]

def number(v):
    try: return float(v)
    except ValueError: return None

def key(row):   # for sorting a bag: numbers by value, strings by text
    return tuple((0, number(v), '') if number(v) is not None else (1, 0, v) for v in row)

def agree(got, want):   # AVG is printed to 4 places and PostgreSQL's numeric is exact, so numbers may differ by 0.00005
    if len(got) != len(want): return False
    for g, w in zip(got, want):
        if len(g) != len(w): return False
        for a, b in zip(g, w):
            if number(a) is not None and number(b) is not None:
                if abs(number(a) - number(b)) > 1e-4: return False
            elif a != b: return False
    return True

def ours(query, frames, baseline):
    with open(os.path.join(WORK, 'q.sql'), 'w') as f: f.write(query)
    logical = tool('sql', 'compile', '--db', os.path.join(WORK, 'db'), '--file', os.path.join(WORK, 'q.sql'), '--json')
    with open(os.path.join(WORK, 'q.json'), 'w') as f: f.write(logical)
    extra = ['--baseline'] if baseline else []
    physical = tool('opt', 'plan', '--db', os.path.join(WORK, 'db'), '--logical', os.path.join(WORK, 'q.json'), '--frames', str(frames), *extra)
    with open(os.path.join(WORK, 'p.json'), 'w') as f: f.write(physical)
    result = json.loads(tool('exec', 'run', '--db', os.path.join(WORK, 'db'), '--plan', os.path.join(WORK, 'p.json'), '--frames', str(frames), '--json'))
    return [[str(v) for v in r] for r in result['rows']], result['peak_frames']

def shell(query, frames):   # the query typed into db, then EXPLAIN ANALYZE of it for the peak frames
    out = run([os.path.join(ROOT, 'db'), '--db', os.path.join(WORK, 'db'), '--frames', str(frames)], query + '\nEXPLAIN ANALYZE ' + query + '\n')
    lines = out.split('\n')
    footer = [i for i, l in enumerate(lines) if l.startswith('(') and ' page I/O ' in l]
    total = [l for l in lines if l.startswith('total: rows ')]
    if len(footer) != 1 or len(total) != 1: raise RuntimeError('db printed no result table or no EXPLAIN ANALYZE total\n' + out)
    rows = [[c.strip() for c in l[1:].split(' | ')] for l in lines[2:footer[0]]]
    return rows, int(total[0].split('peak frames ')[1].split()[0])

def postgres(query):
    out = run(psql + ['-v', 'ON_ERROR_STOP=1', '-X', '-A', '-t', '-F', '\t', '-c', query])
    return [line.split('\t') for line in out.split('\n') if line != '']

def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    data = os.path.join(WORK, 'data')
    run(['python3', os.path.join(ROOT, 'perf', 'data.py'), data, scale])
    tables = ['Dept', 'Emp', 'Proj', 'Works']
    script = open(os.path.join(data, 'schema.sql')).read() + ''.join("LOAD %s FROM '%s';\n" % (t, os.path.join(data, t.lower() + '.csv')) for t in tables)
    script += ''.join('ANALYZE %s;\n' % t for t in tables)
    with open(os.path.join(WORK, 'load.sql'), 'w') as f: f.write(script)
    tool('db', '--db', os.path.join(WORK, 'db'), '--file', os.path.join(WORK, 'load.sql'))
    run(psql + ['-v', 'ON_ERROR_STOP=1', '-q', '-c', 'DROP SCHEMA IF EXISTS public CASCADE; CREATE SCHEMA public;'])
    run(psql + ['-v', 'ON_ERROR_STOP=1', '-q', '-f', os.path.join(data, 'schema.sql')])
    for t in tables: run(psql + ['-v', 'ON_ERROR_STOP=1', '-q', '-c', "\\copy %s FROM '%s' WITH (FORMAT csv)" % (t, os.path.join(data, t.lower() + '.csv'))])
    failures = runs = 0
    for i, q in enumerate(statements(os.path.join(ROOT, 'tests', 'e2e', 'queries.sql')), 1):
        want = postgres(q)
        ordered = 'ORDER BY' in q.upper()
        # unoptimised, a comma join of three or more tables is a cross product of billions of rows; its JOIN ON twin is run instead
        upper = q.upper()
        commas = upper[upper.find(' FROM ') + 6:upper.find(' WHERE ') if ' WHERE ' in upper else len(upper)].count(',')
        modes = ('optimised', 'shell') if commas >= 2 else ('optimised', 'unoptimised', 'shell')
        for frames in (4, 8, 32):
            for mode in modes:
                runs += 1
                try:
                    got, peak = shell(q, frames) if mode == 'shell' else ours(q, frames, mode == 'unoptimised')
                    same = agree(got, want) if ordered else agree(sorted(got, key=key), sorted(want, key=key))
                    if same and peak <= frames: continue
                    detail = 'got %d rows, PostgreSQL %d, peak frames %d\n    got %s\n    want %s' % (len(got), len(want), peak, got[:4], want[:4])
                except RuntimeError as e:
                    detail = str(e)
                failures += 1
                print('FAIL query %d at B=%d %s: %s\n  %s' % (i, frames, mode, q, detail), flush=True)
        print('query %2d: %5d rows from PostgreSQL%s%s' % (i, len(want), ', in order' if ordered else '', '' if 'unoptimised' in modes else ', no unoptimised run'), flush=True)
    print('%d/%d runs agree with PostgreSQL' % (runs - failures, runs))
    shutil.rmtree(WORK)
    sys.exit(1 if failures else 0)

main()
