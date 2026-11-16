#!/usr/bin/env python3
# the hidden workload of the integration checkpoint: loads a dataset into a fresh database, runs every query of
# a SQL file through the db shell, and writes for each the result rows, the page I/O, the time and the estimated
# against actual rows at the root, with its EXPLAIN ANALYZE. Given psql connection arguments, it loads the same
# data into PostgreSQL and checks every result against it. Usage:
#   python3 perf/workload.py DATA_DIR QUERIES.sql [--frames B] [--psql "psql connection arguments"]
# DATA_DIR holds schema.sql (CREATE TABLE statements) and one CSV file per table, named after the table.
import argparse, datetime, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'perf'))
from compare import run, statements, tablesOf, loadPostgres, postgres, agree, sameBag

FOOTER = re.compile(r'^\((\d+) rows?\)  page I/O (\d+) \((\d+) reads, (\d+) writes\)  time ([\d.]+) ms  rows estimated (\d+) actual (\d+)$')


def csvFiles(data, tables):
    names = {f.lower(): os.path.join(data, f) for f in os.listdir(data)}
    missing = [t for t in tables if t.lower() + '.csv' not in names]
    if missing: sys.exit('no CSV file in %s for %s (expected %s)' % (data, ', '.join(missing), ', '.join(t.lower() + '.csv' for t in missing)))
    return {t: names[t.lower() + '.csv'] for t in tables}


def ours(db, frames, query, timeout):
    try:
        r = subprocess.run([os.path.join(ROOT, 'db'), '--db', db, '--frames', str(frames)], input=query + '\nEXPLAIN ANALYZE ' + query + '\n',
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {'error': 'timed out after %d s' % timeout}
    lines = (r.stdout + r.stderr).split('\n')
    errors = [l for l in lines if l.startswith(('error', 'internal error'))]
    if errors: return {'error': errors[0]}
    footer = next((i for i, l in enumerate(lines) if FOOTER.match(l)), None)
    if footer is None: return {'error': 'db printed no result table: ' + ' / '.join(l for l in lines if l)[:300]}
    rows, io, reads, writes, ms, estimated, actual = FOOTER.match(lines[footer]).groups()
    return {'rows': [[c.strip() for c in l[1:].split(' | ')] for l in lines[2:footer]], 'io': int(io), 'reads': int(reads), 'writes': int(writes),
            'ms': float(ms), 'estimated': int(estimated), 'actual': int(actual), 'explain': '\n'.join(lines[footer + 1:]).strip()}


def verdict(query, got, psql):
    try:
        want = postgres(psql, query)
    except RuntimeError as e:
        return 'PostgreSQL error: ' + str(e).strip().split('\n')[-1]
    if 'ORDER BY' in query.upper() and agree(got, want): return 'match'
    if 'ORDER BY' not in query.upper() and sameBag(got, want): return 'match'
    if sameBag(got, want): return 'same rows, ties ordered differently'
    return 'DIFFERS: %d rows here, %d in PostgreSQL' % (len(got), len(want))


def qError(estimated, actual):
    return max(estimated, 1) / max(actual, 1) if estimated >= actual else max(actual, 1) / max(estimated, 1)


def main():
    parser = argparse.ArgumentParser(description='run a query workload and report it, optionally against PostgreSQL')
    parser.add_argument('data')
    parser.add_argument('queries')
    parser.add_argument('--frames', type=int, default=32)
    parser.add_argument('--psql', help='connection arguments, e.g. "psql -h localhost -U me -d scratch"; its tables of the same names are replaced')
    parser.add_argument('--out', default=os.path.join(ROOT, 'perf', 'workload'))
    parser.add_argument('--timeout', type=int, default=600, help='seconds allowed for each query')
    args = parser.parse_args()
    if not os.path.exists(os.path.join(ROOT, 'db')): sys.exit('there is no db program in %s; build it with make first' % ROOT)

    schema = open(os.path.join(args.data, 'schema.sql')).read()
    tables = tablesOf(schema)
    files = csvFiles(args.data, tables)
    queries = statements(args.queries)
    db = os.path.join(args.out, 'db')
    shutil.rmtree(db, ignore_errors=True)
    os.makedirs(args.out, exist_ok=True)

    load = schema + ''.join("LOAD %s FROM '%s';\n" % (t, os.path.abspath(files[t])) for t in tables) + ''.join('ANALYZE %s;\n' % t for t in tables)
    open(os.path.join(args.out, 'load.sql'), 'w').write(load)
    print(run([os.path.join(ROOT, 'db'), '--db', db, '--file', os.path.join(args.out, 'load.sql')]).strip())
    psql = args.psql.split() if args.psql else None
    if psql: loadPostgres(psql, schema, files)

    lines = ['# Workload report', '',
             'Dataset `%s`, queries `%s`, B = %d frames, run %s.' % (args.data, args.queries, args.frames, datetime.datetime.now().strftime('%Y-%m-%d %H:%M')),
             'Time is the engine\'s execution time for the query. Page I/O counts every page read and written, temporary files included.',
             'The q-error is max(estimated / actual, actual / estimated) of the rows at the root.' + (' Each result is checked against PostgreSQL over the same data, comparing strings in the "C" collation, as this engine does.' if psql else ''),
             '', '| # | rows | page I/O | reads | writes | time (ms) | estimated rows | actual rows | q-error |' + (' PostgreSQL |' if psql else ''),
             '|---:|---:|---:|---:|---:|---:|---:|---:|---:|' + ('---|' if psql else '')]
    failures = 0
    for i, q in enumerate(queries, 1):
        r = ours(db, args.frames, q, args.timeout)
        with open(os.path.join(args.out, 'q%02d.txt' % i), 'w') as f: f.write(q + '\n\n' + r.get('explain', r.get('error', '')) + '\n')
        if 'error' in r:
            failures += 1
            lines.append('| %d | %s |' % (i, r['error'].replace('|', '\\|')) + ' |' * (8 if psql else 7))
            print('query %2d: %s' % (i, r['error']), flush=True)
            continue
        check = verdict(q, r['rows'], psql) if psql else None
        if check and check != 'match' and not check.startswith('same rows'): failures += 1
        lines.append('| %d | %d | %d | %d | %d | %.3f | %d | %d | %.2f |' % (i, len(r['rows']), r['io'], r['reads'], r['writes'], r['ms'], r['estimated'], r['actual'], qError(r['estimated'], r['actual']))
                     + (' %s |' % check if psql else ''))
        print('query %2d: %6d rows, page I/O %7d, %9.3f ms, estimated %d%s' % (i, len(r['rows']), r['io'], r['ms'], r['estimated'], ', ' + check if check else ''), flush=True)
    lines += ['', '## Queries', ''] + ['%d. `%s`' % (i, ' '.join(q.split())) for i, q in enumerate(queries, 1)]
    lines += ['', 'Each query\'s physical plan with estimated and actual rows and I/O per operator (EXPLAIN ANALYZE) is in `q01.txt` to `q%02d.txt` beside this report.' % len(queries)]
    open(os.path.join(args.out, 'report.md'), 'w').write('\n'.join(lines) + '\n')
    shutil.rmtree(db)
    print('%d of %d queries %s; report in %s' % (len(queries) - failures, len(queries), 'agree with PostgreSQL' if psql else 'ran', os.path.join(args.out, 'report.md')))
    sys.exit(1 if failures else 0)


main()
