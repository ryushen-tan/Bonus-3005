#!/usr/bin/env python3
# prints the report tables and slopes, writes perf/time.svg
import csv, html, math, sys

rows = list(csv.DictReader(open(sys.argv[1]), delimiter='\t'))
series = {}
for r in rows:
    if r['k'] == '1' and r['n'] == r['m'] and int(r['n']) not in [n for n, _ in series.get(r['query'], [])]:
        series.setdefault(r['query'], []).append((int(r['n']), float(r['time'])))

def slope(pts):   # least squares slope of log time against log n
    xs = [math.log(n) for n, _ in pts]; ys = [math.log(t) for _, t in pts]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)

print('| query | n | m | k | comparisons | wall time (s) | output tuples | ns per comparison |')
print('|---|---|---|---|---|---|---|---|')
for r in rows:
    ns = '%.2f' % (float(r['time']) / int(r['comparisons']) * 1e9) if int(r['comparisons']) else '-'
    print('| %s | %s | %s | %s | %s | %s | %s | %s |' % (r['query'], r['n'], r['m'], r['k'], r['comparisons'], r['time'], r['tuples'], ns))
for q, s in series.items():
    print('%s slope %.3f steps %s' % (q, slope(s), ' '.join('%.2f' % math.log2(s[i + 1][1] / s[i][1]) for i in range(len(s) - 1))))

W, H, M = 640, 420, 60
pts = [p for s in series.values() for p in s]
nlo, nhi = math.log10(min(n for n, _ in pts)), math.log10(max(n for n, _ in pts))
tlo, thi = math.floor(math.log10(min(t for _, t in pts))), math.ceil(math.log10(max(t for _, t in pts)))
X = lambda n: M + (math.log10(n) - nlo) / (nhi - nlo) * (W - 2 * M)
Y = lambda t: H - M - (math.log10(t) - tlo) / (thi - tlo) * (H - 2 * M)
svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" font-family="sans-serif" font-size="11">' % (W, H), '<rect width="100%" height="100%" fill="white"/>']
for n, _ in next(iter(series.values())):
    svg.append('<line x1="%.0f" y1="%d" x2="%.0f" y2="%d" stroke="#ddd"/><text x="%.0f" y="%d" text-anchor="middle">%d</text>' % (X(n), M, X(n), H - M, X(n), H - M + 15, n))
for e in range(tlo, thi + 1):
    svg.append('<line x1="%d" y1="%.0f" x2="%d" y2="%.0f" stroke="#ddd"/><text x="%d" y="%.0f" text-anchor="end">1e%d s</text>' % (M, Y(10 ** e), W - M, Y(10 ** e), M - 5, Y(10 ** e) + 4, e))
for i, (q, s) in enumerate(series.items()):
    c = ['#c33', '#36c', '#393'][i]
    svg.append('<polyline fill="none" stroke="%s" stroke-width="2" points="%s"/>' % (c, ' '.join('%.0f,%.0f' % (X(n), Y(t)) for n, t in s)))
    svg += ['<circle cx="%.0f" cy="%.0f" r="3" fill="%s"/>' % (X(n), Y(t), c) for n, t in s]
    svg.append('<text x="%d" y="%d" fill="%s">%s slope %.2f</text>' % (M + 8, M + 14 * (i + 1), c, html.escape(q), slope(s)))
svg.append('<text x="%d" y="%d" text-anchor="middle">n = m tuples per relation, log-log</text></svg>' % (W // 2, H - 8))
open('perf/time.svg', 'w').write('\n'.join(svg))
