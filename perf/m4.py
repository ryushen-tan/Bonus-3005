#!/usr/bin/env python3
# Module 4 measurements: loads the company dataset, runs the twenty queries of perf/m4_queries.sql through
# sql, opt and exec, and prints the tables of REPORT_M4.md
import json, os, shutil, statistics, subprocess, sys

OUT = 'perf/m4'
DB = OUT + '/db'
FRAMES = 16

def run(*args):
    r = subprocess.run(list(args), capture_output=True, text=True)
    if r.returncode != 0: sys.exit(' '.join(args) + '\n' + r.stdout + r.stderr)
    return r.stdout

def statements(path):
    text = ''.join(l for l in open(path) if not l.lstrip().startswith('--'))
    return [s.strip() + ';' for s in text.split(';') if s.strip()]

def logical(sql):
    open(OUT + '/q.sql', 'w').write(sql)
    open(OUT + '/q.json', 'w').write(run('./sql', 'compile', '--db', DB, '--file', OUT + '/q.sql', '--json'))

def physical(frames, *flags):
    plan = run('./opt', 'plan', '--db', DB, '--logical', OUT + '/q.json', '--frames', str(frames), *flags)
    open(OUT + '/p.json', 'w').write(plan)
    return json.loads(plan)

def execute(frames):
    totals = json.loads(run('./exec', 'run', '--db', DB, '--plan', OUT + '/p.json', '--frames', str(frames), '--json'))['totals']
    trace = run('./exec', 'trace', '--db', DB, '--plan', OUT + '/p.json', '--frames', str(frames))
    actual = [int(l.split('tuples_out=')[1].split()[0]) for l in trace.splitlines() if 'tuples_out=' in l and not l.startswith('total')]
    return totals['page_reads'] + totals['page_writes'], actual

# the nodes below a limit stop when the limit has enough rows, so their counts are not their full output
def limited(p, below=False):
    out = [below]
    for k in ('child', 'left', 'right'):
        if k in p: out += limited(p[k], below or p['op'] == 'limit')
    return out

def nodes(p):
    out = [p]
    for k in ('child', 'left', 'right'):
        if k in p: out += nodes(p[k])
    return out

def shape(p):
    kids = [shape(p[k]) for k in ('child', 'left', 'right') if k in p]
    name = p['op'] + ('(' + p['table'] + ')' if 'table' in p else '')
    return name if not kids else name + '[' + ','.join(kids) + ']'

def joins(p): return ' '.join(n['op'].replace('nested_loop_join', 'BNL').replace('hash_join', 'HJ').replace('sort_merge_join', 'SMJ') for n in nodes(p) if 'join' in n['op'])

def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    run('python3', 'perf/data.py', OUT + '/data', '5')
    tables = ['Dept', 'Emp', 'Proj', 'Works']
    script = open(OUT + '/data/schema.sql').read() + ''.join("LOAD %s FROM '%s/data/%s.csv';\nANALYZE %s;\n" % (t, OUT, t.lower(), t) for t in tables)
    open(OUT + '/load.sql', 'w').write(script)
    print('```\n' + run('./db', '--db', DB, '--file', OUT + '/load.sql') + '```\n')
    queries = statements('perf/m4_queries.sql')

    print('## 1 Estimation quality and 2 does optimisation pay (B = %d)\n' % FRAMES)
    print('| query | nodes | median q-error | worst q-error | worst node | optimised I/O | joins chosen | unoptimised I/O | unoptimised / optimised |')
    print('|---|---|---|---|---|---|---|---|---|')
    errors, ratios, total_opt, excluded = [], [], 0, 0
    for i, q in enumerate(queries, 1):
        logical(q)
        plan = physical(FRAMES)
        io, actual = execute(FRAMES)
        est = [n['est_rows'] for n in nodes(plan)]
        full = [k for k, cut in enumerate(limited(plan)) if not cut]
        qs = [max(est[k], max(actual[k], 1)) / max(1, min(est[k], max(actual[k], 1))) for k in full]
        excluded += len(est) - len(full)
        worst = max(range(len(qs)), key=lambda k: qs[k])
        worstNode = full[worst]
        errors += qs
        physical(FRAMES, '--baseline')
        base, _ = execute(FRAMES)
        ratios.append(base / io)
        total_opt += io
        print('| %d | %d | %.2f | %.2f | %s est %d actual %d | %d | %s | %d | %.2f |' % (i, len(qs), statistics.median(qs), qs[worst], nodes(plan)[worstNode]['op'], est[worstNode], actual[worstNode], io, joins(plan) or '-', base, base / io))
    print('\nAcross all %d nodes (%d nodes below a LIMIT left out): median q-error %.2f, worst %.2f. Unoptimised / optimised I/O: median %.2f, range %.2f to %.2f.\n' %
          (len(errors), excluded, statistics.median(errors), max(errors), statistics.median(ratios), min(ratios), max(ratios)))

    print('## 3 Which rule mattered (B = %d)\n' % FRAMES)
    print('| rules turned off | total I/O of the twenty queries | against all rules on |')
    print('|---|---|---|')
    print('| none | %d | 1.00 |' % total_opt)
    for rule in '1234567':
        total = 0
        for q in queries:
            logical(q)
            physical(FRAMES, '--without-rules', rule)
            total += execute(FRAMES)[0]
        print('| %s | %d | %.2f |' % (rule, total, total / total_opt))

    print('\n## The DP table of query 9, a four relation join, at B = %d\n' % FRAMES)
    logical(queries[8])
    print('```\n' + queries[8] + '\n' + run('./opt', 'dp-table', '--db', DB, '--logical', OUT + '/q.json', '--frames', str(FRAMES)) + '```')
    print('```\n' + run('./opt', 'explain', '--db', DB, '--logical', OUT + '/q.json', '--frames', str(FRAMES)) + '```')
    print('\n## 4 Where it breaks: level and salary are correlated\n')
    shutil.rmtree(DB)
    run('python3', 'perf/data.py', OUT + '/big', '20')
    open(OUT + '/load.sql', 'w').write(open(OUT + '/big/schema.sql').read() + ''.join("LOAD %s FROM '%s/big/%s.csv';\nANALYZE %s;\n" % (t, OUT, t.lower(), t) for t in tables))
    print('```\n' + run('./db', '--db', DB, '--file', OUT + '/load.sql') + '```\n')
    bad = 'SELECT e.name, e.city, e.age, e.did, e.mgr, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid WHERE e.level = 8 AND e.salary > 160000;'
    good = 'SELECT e.name, e.city, e.age, e.did, e.mgr, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid WHERE e.salary > 160000;'
    print('| B | with level = 8: join, estimated Emp rows, actual, I/O | salary only: join, estimated, actual, I/O | I/O ratio |')
    print('|---|---|---|---|')
    for b in [4, 6, 8, 12, 16, 32]:
        cells = []
        for sql in (bad, good):
            logical(sql)
            plan = physical(b)
            io, actual = execute(b)
            filt = [k for k, n in enumerate(nodes(plan)) if n['op'] == 'filter'][0]
            cells.append((joins(plan), nodes(plan)[filt]['est_rows'], actual[filt], io))
        print('| %d | %s, %d, %d, %d | %s, %d, %d, %d | %.2f |' % (b, *cells[0], *cells[1], cells[0][3] / cells[1][3]))
    for sql in (bad, good):
        logical(sql)
        print('\n```\n' + sql + '\n' + run('./opt', 'explain', '--db', DB, '--logical', OUT + '/q.json', '--frames', '4') + '```')
        physical(4)
        print('```\n' + run('./exec', 'trace', '--db', DB, '--plan', OUT + '/p.json', '--frames', '4') + '```')
    shutil.rmtree(OUT + '/big')
    shutil.rmtree(DB)
    shutil.rmtree(OUT + '/data')
    for f in ['q.sql', 'q.json', 'p.json', 'load.sql']: os.remove(OUT + '/' + f)

main()
