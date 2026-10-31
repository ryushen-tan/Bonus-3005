#!/usr/bin/env python3
# Module 1 measurements: writes the five traces, runs ./storage, prints the tables for REPORT_M1.md
import json, os, shutil, subprocess, sys

OUT = 'perf/m1'
DB = OUT + '/db'

def storage(*args):
    r = subprocess.run(['./storage', *args], capture_output=True, text=True)
    if r.returncode != 0: sys.exit('storage %s failed: %s' % (' '.join(args), r.stdout + r.stderr))
    return r.stdout

def lcg(seed):   # a fixed generator so the traces are the same on every machine
    while True:
        seed = (seed * 1103515245 + 12345) % 2 ** 31
        yield seed

def traces():
    r = lcg(42)
    uniform = [next(r) % 200 for _ in range(1000)]
    hot = []
    for _ in range(1000):
        hot.append(next(r) % 40 if next(r) % 100 < 80 else 40 + next(r) % 160)
    scan = [p for _ in range(10) for p in range(80)]
    mixed = []
    for i in range(300):
        mixed += [i % 10, 100 + i]
    loop = [p for outer in range(20) for p in [outer] + list(range(100, 140))]
    return {'1 uniform over 200 pages': uniform, '2 80/20 skew over 200 pages': hot, '3 sequential scan of 80 pages, ten times': scan,
            '4 hot set of 10 pages mixed with a 300 page scan': mixed, '5 nested loop: 20 outer pages, 40 page inner scan': loop}

def main():
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT + '/traces')
    print('## Policies\n')
    print('| trace | accesses | distinct pages | B | LRU hits | LRU hit rate | CLOCK hits | CLOCK hit rate |')
    print('|---|---|---|---|---|---|---|---|')
    for i, (name, ids) in enumerate(traces().items(), 1):
        path = '%s/traces/trace%d.txt' % (OUT, i)
        open(path, 'w').write(''.join('%d\n' % p for p in ids))
        for b in [4, 16, 64, 256]:
            res = {}
            for policy in ['lru', 'clock']:
                res[policy] = json.loads(storage('trace', '--db', DB, '--file', path, '--frames', str(b), '--policy', policy, '--json'))
            print('| %s | %d | %d | %d | %d | %.3f | %d | %.3f |' % (name, len(ids), len(set(ids)), b, res['lru']['hits'], res['lru']['hits'] / len(ids),
                                                         res['clock']['hits'], res['clock']['hits'] / len(ids)))

    print('\n## Capacity\n')
    print('| schema | record bytes | computed per page | measured on page 1 | measured tuples / data pages |')
    print('|---|---|---|---|---|')
    cases = [('Ints', 'a:INT,b:INT,c:INT', lambda i: '%d,%d,%d' % (i, i, i), 12),
             ('Short', 'a:INT,b:VARCHAR(20),c:INT', lambda i: '%d,bob,%d' % (i, i), 13),
             ('Long', 'a:INT,b:VARCHAR(200)', lambda i: '%d,%s' % (i, 'x' * 200), 206)]
    for name, schema, line, length in cases:
        storage('create-table', '--db', DB, '--name', name, '--schema', schema)
        open(OUT + '/c.csv', 'w').write(''.join(line(i) + '\n' for i in range(5000)))
        loaded = storage('load', '--db', DB, '--table', name, '--file', OUT + '/c.csv')
        pages = int(loaded.split(' on ')[1].split()[0])
        first = int([l for l in storage('layout', '--db', DB, '--table', name, '--page', '1').splitlines() if 'slot_count' in l][0].split()[-1])
        print('| %s | %d | floor(4088 / %d) = %d | %d | 5000 / %d = %.2f |' % (schema, length, length + 4, 4088 // (length + 4), first, pages, 5000 / pages))

    print('\n## Scan cost\n')
    storage('create-table', '--db', DB, '--name', 'Big', '--schema', 'a:INT,b:VARCHAR(20),c:INT')
    open(OUT + '/big.csv', 'w').write(''.join('%d,bob,%d\n' % (i, i) for i in range(240 * 500)))
    print(storage('load', '--db', DB, '--table', 'Big', '--file', OUT + '/big.csv').strip() + '\n')
    print('| B | hits | misses | evictions | reads | writes |')
    print('|---|---|---|---|---|---|')
    for b in [2, 8, 64]:
        s = json.loads(storage('scan', '--db', DB, '--table', 'Big', '--frames', str(b), '--limit', '1000000', '--json'))['stats']
        print('| %d | %d | %d | %d | %d | %d |' % (b, s['hits'], s['misses'], s['evictions'], s['reads'], s['writes']))
    shutil.rmtree(DB)

main()
