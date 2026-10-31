#!/usr/bin/env python3
# Module 2 measurements: builds the tables with ./storage, runs plans with ./exec, prints the tables for
# REPORT_M2.md and writes perf/m2/crossover_b.svg and perf/m2/crossover_size.svg
import html, json, math, os, shutil, subprocess, sys

OUT = 'perf/m2'
DB = OUT + '/db'

def run(*args):
    r = subprocess.run(list(args), capture_output=True, text=True)
    if r.returncode != 0: sys.exit(' '.join(args) + '\n' + r.stdout + r.stderr)
    return r.stdout

def table(name, schema, rows):
    run('./storage', 'create-table', '--db', DB, '--name', name, '--schema', schema)
    path = '%s/%s.csv' % (OUT, name)
    open(path, 'w').write(''.join(','.join(map(str, r)) + '\n' for r in rows))
    out = run('./storage', 'load', '--db', DB, '--table', name, '--file', path)
    os.remove(path)
    return int(out.split(' on ')[1].split()[0])

def plan(p, frames):
    path = OUT + '/plan.json'
    open(path, 'w').write(json.dumps(p))
    totals = json.loads(run('./exec', 'run', '--db', DB, '--plan', path, '--frames', str(frames), '--json'))
    trace = run('./exec', 'trace', '--db', DB, '--plan', path, '--frames', str(frames))
    return totals, trace

def scan(t): return {'op': 'seqscan', 'table': t}
def join(alg, l, r, cond): return {'op': alg, 'condition': cond, 'left': scan(l), 'right': scan(r)}

def passes(n, b): return 1 if math.ceil(n / b) <= 1 else 1 + math.ceil(math.log(math.ceil(n / b)) / math.log(b - 1))
def sort_cost(m, b): return 2 * m * (passes(m, b) - 1) + m
def formula(alg, m, n, b):
    if alg == 'nested_loop_join': return m + math.ceil(m / (b - 2)) * n
    if alg == 'sort_merge_join': return sort_cost(m, b) + sort_cost(n, b) + m + n
    return 3 * (m + n)

ALGS = ['nested_loop_join', 'sort_merge_join', 'hash_join']
SHORT = {'nested_loop_join': 'BNL', 'sort_merge_join': 'SMJ', 'hash_join': 'HJ'}

def svg(path, title, xlabel, xs, series, logx):
    W, H, M = 640, 420, 70
    ys = [y for s in series.values() for y in s if y > 0]
    lo, hi = math.floor(math.log10(min(ys))), math.ceil(math.log10(max(ys)))
    xlo, xhi = (math.log10(xs[0]), math.log10(xs[-1])) if logx else (xs[0], xs[-1])
    X = lambda x: M + ((math.log10(x) if logx else x) - xlo) / (xhi - xlo) * (W - 2 * M)
    Y = lambda y: H - M - (math.log10(y) - lo) / (hi - lo) * (H - 2 * M)
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" font-family="sans-serif" font-size="11">' % (W, H), '<rect width="100%" height="100%" fill="white"/>']
    for x in xs: out.append('<line x1="%.0f" y1="%d" x2="%.0f" y2="%d" stroke="#ddd"/><text x="%.0f" y="%d" text-anchor="middle">%d</text>' % (X(x), M, X(x), H - M, X(x), H - M + 15, x))
    for e in range(lo, hi + 1): out.append('<line x1="%d" y1="%.0f" x2="%d" y2="%.0f" stroke="#ddd"/><text x="%d" y="%.0f" text-anchor="end">1e%d</text>' % (M, Y(10 ** e), W - M, Y(10 ** e), M - 5, Y(10 ** e) + 4, e))
    for i, (name, s) in enumerate(series.items()):
        c = ['#c33', '#36c', '#393'][i]
        out.append('<polyline fill="none" stroke="%s" stroke-width="2" points="%s"/>' % (c, ' '.join('%.0f,%.0f' % (X(x), Y(y)) for x, y in zip(xs, s))))
        out += ['<circle cx="%.0f" cy="%.0f" r="3" fill="%s"/>' % (X(x), Y(y), c) for x, y in zip(xs, s)]
        out.append('<text x="%d" y="%d" fill="%s">%s</text>' % (W - M - 150, M + 14 * (i + 1), c, html.escape(name)))
    out.append('<text x="%d" y="%d" text-anchor="middle">%s</text>' % (W // 2, H - 25, html.escape(xlabel)))
    out.append('<text x="%d" y="30" text-anchor="middle" font-size="13">%s</text>' % (W // 2, html.escape(title)))
    out.append('<text x="15" y="%d" transform="rotate(-90 15 %d)" text-anchor="middle">measured page I/O, log scale</text></svg>' % (H // 2, H // 2))
    open(path, 'w').write('\n'.join(out) + '\n')

def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    pad = 'x' * 40
    sizes = {}
    for n in [1500, 6000, 24000]:
        sizes[n] = (table('R%d' % n, 'a:INT,b:INT,pad:VARCHAR(40)', [(i, (i * 7919) % n, pad) for i in range(n)]),
                    table('S%d' % n, 'b:INT,c:INT,pad:VARCHAR(40)', [((i * 104729) % n, i, pad) for i in range(n)]))

    print('## 1 Cost against formula\n')
    print('Data pages M and N exclude the directory page each scan also reads. Measured is the plan total: the two scans plus the join\'s own reads and writes.\n')
    print('| join | tuples per side | M = N pages | B | measured reads | measured writes | measured total | formula | measured / formula | output |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    for n, (m, nn) in sizes.items():
        for alg in ALGS:
            for b in [4, 16, 64, 256]:
                t, _ = plan(join(alg, 'R%d' % n, 'S%d' % n, 'R%d.b = S%d.b' % (n, n)), b)
                tot = t['totals']
                f = formula(alg, m, nn, b)
                total = tot['page_reads'] + tot['page_writes']
                print('| %s | %d | %d | %d | %d | %d | %d | %d | %.2f | %d |' % (SHORT[alg], n, m, b, tot['page_reads'], tot['page_writes'], total, f, total / f, tot['tuples_out']))

    print('\n## 2 Sort passes\n')
    print('| N pages | B | runs after pass 0 | measured passes | 1 + ceil(log(ceil(N/B)) / log(B-1)) | sort reads | sort writes | scan reads |')
    print('|---|---|---|---|---|---|---|---|')
    for n_tuples, b in [(1500, 4), (6000, 4), (6000, 8), (24000, 16), (24000, 4), (24000, 64)]:
        n = sizes[n_tuples][0]
        _, trace = plan({'op': 'sort', 'keys': [['R%d.b' % n_tuples, 'desc']], 'child': scan('R%d' % n_tuples)}, b)
        sort_line, scan_line = trace.splitlines()[0], trace.splitlines()[1]
        measured = int(sort_line.split('passes=')[1])
        field = lambda line, name: int(line.split(name + '=')[1].split()[0])
        print('| %d | %d | %d | %d | %d | %d | %d | %d |' % (n, b, math.ceil(n / b), measured, passes(n, b), field(sort_line, 'reads'), field(sort_line, 'writes'), field(scan_line, 'reads')))
    twenty = table('Twenty', 'a:INT,pad:VARCHAR(40)', [((i * 7919) % 6600, pad) for i in range(6600)])

    print('\n## 3 Crossover\n')
    print('Varying B with R6000 and S6000 fixed (M = N = %d pages):\n' % sizes[6000][0])
    print('| B | BNL I/O | SMJ I/O | HJ I/O | cheapest |')
    print('|---|---|---|---|---|')
    bs = [4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128]
    by_b = {a: [] for a in ALGS}
    for b in bs:
        row = {}
        for alg in ALGS:
            t, _ = plan(join(alg, 'R6000', 'S6000', 'R6000.b = S6000.b'), b)
            row[alg] = t['totals']['page_reads'] + t['totals']['page_writes']
            by_b[alg].append(row[alg])
        print('| %d | %d | %d | %d | %s |' % (b, row[ALGS[0]], row[ALGS[1]], row[ALGS[2]], SHORT[min(ALGS, key=lambda a: row[a])]))
    svg(OUT + '/crossover_b.svg', 'Joins of R6000 and S6000 against the frame budget B', 'B, frames (log scale)', bs, {SHORT[a]: by_b[a] for a in ALGS}, True)
    print('\nVarying the size of both relations with B = 16 fixed:\n')
    print('| tuples per side | M = N pages | BNL I/O | SMJ I/O | HJ I/O | cheapest |')
    print('|---|---|---|---|---|---|')
    ns = [300, 600, 1200, 2400, 4800, 9600]
    by_n = {a: [] for a in ALGS}
    for n in ns:
        m = table('VR%d' % n, 'a:INT,b:INT,pad:VARCHAR(40)', [(i, (i * 7919) % n, pad) for i in range(n)])
        table('VS%d' % n, 'b:INT,c:INT,pad:VARCHAR(40)', [((i * 104729) % n, i, pad) for i in range(n)])
        row = {}
        for alg in ALGS:
            t, _ = plan(join(alg, 'VR%d' % n, 'VS%d' % n, 'VR%d.b = VS%d.b' % (n, n)), 16)
            row[alg] = t['totals']['page_reads'] + t['totals']['page_writes']
            by_n[alg].append(row[alg])
        print('| %d | %d | %d | %d | %d | %s |' % (n, m, row[ALGS[0]], row[ALGS[1]], row[ALGS[2]], SHORT[min(ALGS, key=lambda a: row[a])]))
    svg(OUT + '/crossover_size.svg', 'Joins at B = 16 against relation size', 'tuples per relation (log scale)', ns, {SHORT[a]: by_n[a] for a in ALGS}, True)

    print('\n## 4 Skew\n')
    print('| data | B | partitions, pages left/right | overflowed | reads | writes | comparisons | output |')
    print('|---|---|---|---|---|---|---|---|')
    for kind in ['uniform', 'skewed']:
        hot = lambda i: kind == 'skewed' and i % 5 < 2
        table('K%s' % kind, 'k:INT,v:INT,pad:VARCHAR(40)', [(7 if hot(i) else (i * 7919) % 6000, i, pad) for i in range(6000)])
        table('L%s' % kind, 'k:INT,w:INT,pad:VARCHAR(40)', [(7 if hot(i) else (i * 104729) % 6000, i, pad) for i in range(6000)])
        for b in [16]:
            t, trace = plan(join('hash_join', 'K%s' % kind, 'L%s' % kind, 'K%s.k = L%s.k' % (kind, kind)), b)
            line = trace.splitlines()[0]
            parts = line.split('pages_left/right=')[1].split(' ')[0]
            print('| %s | %d | %s | %s | %d | %d | %d | %d |' % (kind, b, parts, line.split('overflowed=')[1].strip(), t['totals']['page_reads'], t['totals']['page_writes'], t['totals']['comparisons'], t['totals']['tuples_out']))

    print('\n## 5 The budget\n')
    t, trace = plan({'op': 'sort', 'keys': [['Twenty.a', 'asc']], 'child': scan('Twenty')}, 4)
    print('Sort of Twenty, %d data pages, at B = 4 (20 x B = 80): %d tuples out, peak frames %d.\n' % (twenty, t['totals']['tuples_out'], t['peak_frames']))
    print('```\n' + trace + '```')

    print('\n## 6 What materialising an input costs\n')
    print('A hash group_by over a hash join: the join holds memory, so the group_by writes its output to a temporary file before taking memory itself.\n')
    grouped = {'op': 'group_by', 'mode': 'hash', 'keys': ['S6000.c'], 'aggregates': [['count', '*']], 'child': join('hash_join', 'R6000', 'S6000', 'R6000.b = S6000.b')}
    for b in [8, 16]:
        t, trace = plan(grouped, b)
        print('B = %d, peak frames %d:\n\n```\n%s```\n' % (b, t['peak_frames'], trace))
    shutil.rmtree(DB)
    os.remove(OUT + '/plan.json')

main()
