#!/usr/bin/env python3
# writes the company dataset as CSV files and its CREATE TABLE statements: python3 perf/data.py OUTDIR SCALE
# level and salary are strongly correlated (salary is about 20000 per level), and Works has one hot project
import os, sys

out, scale = sys.argv[1], int(sys.argv[2])
os.makedirs(out, exist_ok=True)
seed = 20261030
def rnd(n):
    global seed
    seed = (seed * 6364136223846793005 + 1442695040888963407) % 2 ** 64
    return (seed >> 33) % n

cities = ['Ottawa', 'Toronto', 'Montreal', 'Kanata', 'Waterloo']
emps, depts, projs = 1000 * scale, 10 * scale, 100 * scale
schema = {
    'Dept': 'did INT, title VARCHAR(20), budget INT, city VARCHAR(20)',
    'Emp': 'eid INT, name VARCHAR(20), age INT, did INT, salary INT, mgr INT, city VARCHAR(20), level INT',
    'Proj': 'pid INT, did INT, name VARCHAR(20), budget INT',
    'Works': 'eid INT, pid INT, hours INT',
}
rows = {'Dept': [], 'Emp': [], 'Proj': [], 'Works': []}
for d in range(depts):
    rows['Dept'].append([d, 'dept%d' % d, rnd(100000), cities[d % len(cities)]])
for e in range(emps):
    level = 1 + rnd(8)
    did = rnd(depts)
    rows['Emp'].append([e, 'e%d' % e, 20 + rnd(46), did, level * 20000 + rnd(15000), rnd(emps), cities[did % len(cities)], level])
for p in range(projs):
    rows['Proj'].append([p, rnd(depts), 'p%d' % rnd(projs // 2), rnd(500000)])
for w in range(3 * emps):
    rows['Works'].append([rnd(emps), 7 if rnd(10) < 3 else rnd(projs), 1 + rnd(40)])

for name, table in rows.items():
    with open(os.path.join(out, name.lower() + '.csv'), 'w') as f:
        f.write(''.join(','.join(str(v) for v in r) + '\n' for r in table))
with open(os.path.join(out, 'schema.sql'), 'w') as f:
    f.write(''.join('CREATE TABLE %s (%s);\n' % (n, s) for n, s in schema.items()))
