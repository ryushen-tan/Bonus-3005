# REPORT_M4.md: optimizer measurements

Machine: Apple M5 Pro, 24 GB, macOS 26.5.2, Apple clang 21.0.0, `-std=c++17 -O2`. Every number comes
from `python3 perf/m4.py`, which writes `perf/m4/results.md` (the tables below, with the full plans) in
about 25 seconds. It generates the data with `perf/data.py`, loads and analyses it through the `db`
shell, and runs every query through `sql compile`, `opt plan` and `exec run`/`exec trace`.

**Data.** The twenty supplied queries and dataset were not on Brightspace when I did this, so I wrote a
company dataset and a twenty query workload (`perf/m4_queries.sql`) to stand in for them.
`perf/data.py OUT 5` writes Dept (50 rows, 1 page), Emp (5000 rows, 54 pages), Proj (500, 3) and Works
(15000, 59). Two columns are strongly correlated on purpose: `Emp.level` is uniform on 1 to 8, and
`Emp.salary` is 20000 · level plus up to 15000. And 30% of Works rows are for project 7, a skewed join key.
All four tables are ANALYZEd with 20 buckets. The same generator and queries are what
`tests/e2e/run.py` checks against PostgreSQL, through the four tools chained by file and through the `db`
shell; all 390 runs agree, at scale 1 and at scale 5.

The workload's multi-way joins are written with JOIN ON. The unoptimised plan (`opt plan --baseline`:
joins in written order, every join a nested loop, nothing pushed down) of a comma join is a cross
product, which for four tables here would be billions of rows.

## 1 Estimation quality

Estimated rows (`est_rows` in the physical plan) against actual rows (`tuples_out` in `exec trace`) at
every node, B = 16. q-error = max(est, actual) / min(est, actual), with actual 0 counted as 1. Nodes
below a LIMIT are left out (23 of 184), because the limit stops pulling, so they never produce their full
output; for example, the sort under `LIMIT 5` in query 10 hands over 5 of its 2000 rows.

| query | nodes | median q-error | worst q-error | worst node |
|---|---|---|---|---|
| 1 `age > 60` | 3 | 1.16 | 1.16 | project est 625, actual 540 |
| 2 `did = 7` | 4 | 1.25 | 1.51 | project est 146, actual 97 |
| 3 `level = 8 AND salary > 160000` | 4 | 4.91 | 8.82 | project est 72, actual 635 |
| 4 | 9 | 1.00 | 1.02 | project est 2700, actual 2757 |
| 5 | 10 | 1.00 | 1.16 | project est 625, actual 540 |
| 6 | 1 | 1.00 | 1.00 | limit est 10, actual 10 |
| 7 self join | 9 | 1.00 | 1.15 | project est 250, actual 217 |
| 8 subquery in FROM | 5 | 1.00 | 1.47 | rename est 34, actual 50 |
| 9 four tables | 19 | 1.00 | 1.34 | project est 375, actual 279 |
| 10 | 1 | 1.00 | 1.00 | limit est 5, actual 5 |
| 11 | 6 | 1.00 | 1.11 | sort est 334, actual 301 |
| 12 `p.pid = 7` | 10 | 1.00 | **152.27** | project est 30, actual 4568 |
| 13 | 10 | 1.00 | 1.00 | sort est 40, actual 40 |
| 14 | 9 | 1.00 | 5.00 | distinct est 25, actual 5 |
| 15 `e.city = d.city` | 9 | 1.00 | 8.62 | project est 13, actual 112 |
| 16 | 10 | 1.01 | 1.06 | group_by est 50, actual 47 |
| 17 correlated, joined | 9 | 1.00 | 8.82 | project est 72, actual 635 |
| 18 | 7 | 1.00 | 2.00 | union est 10, actual 5 |
| 19 | 15 | 1.04 | 3.93 | project est 1140, actual 290 |
| 20 | 11 | 1.02 | 5.26 | project est 1000, actual 190 |

**Across all 161 nodes: median q-error 1.00, worst 152.27.** The median is 1 because most nodes are
scans, renames and projections over well estimated single table predicates, and ANALYZE's statistics are
exact. The interesting part is the worst cases. Each comes from one of the three assumptions
DESIGN_M4.md names:

- **Query 12, 152×: containment fails on a skewed key.** `Works JOIN Proj ON w.pid = p.pid WHERE p.pid =
  7` is estimated as 15000 · 1 / max(500, 500) = 30, assuming every project has an equal share of Works.
  Project 7 has 30% of Works, so 4568 rows. The filter on Proj is estimated exactly (1 row). The error is
  entirely in the join, which knows the number of distinct values of `w.pid` but not that one of them
  is frequent. The histogram of `Works.pid` does show it: the bucket counts around 7 are huge. But the
  join formula does not look at histograms.
- **Queries 3 and 17, 8.8×: independence fails** on `level = 8 AND salary > 160000`, section 4.
- **Query 15, 8.6×: a two column join predicate.** `e.did = d.did AND e.city = d.city` multiplies two
  join selectivities, as if a matching department were no more likely to be in the employee's city. In
  this data a department's city determines its employees' city, so the second predicate removes nothing,
  and the estimate is 13 against 112.
- **Queries 14 and 18, 5× and 2×: distinct counts of a combination.** DISTINCT and UNION are capped by
  the product of their columns' distinct values (5 cities × 5 cities = 25), but the actual combinations
  are 5. Again a correlation: department and employee city are the same column in disguise.
- **Queries 19 and 20, 4× and 5× over**: joins of filtered inputs, where the filter removes rows that
  would have matched more often than average. Uniformity across the join key.

## 2 Does optimisation pay

Actual page I/O at B = 16 of the optimised plan and of the unoptimised one.

| query | optimised I/O | joins chosen | unoptimised I/O | unoptimised / optimised |
|---|---|---|---|---|
| 1 | 55 | - | 55 | 1.00 |
| 2 | 55 | - | 55 | 1.00 |
| 3 | 55 | - | 55 | 1.00 |
| 4 | 57 | BNL | 63 | 1.11 |
| 5 | 57 | BNL | 63 | 1.11 |
| 6 | 57 | - | 153 | 2.68 |
| 7 | 110 | BNL | 272 | 2.47 |
| 8 | 55 | - | 163 | 2.96 |
| 9 | 127 | BNL BNL BNL | 1413 | 11.13 |
| 10 | 155 | BNL BNL | 1082 | 6.98 |
| 11 | 64 | - | 182 | 2.84 |
| 12 | 64 | BNL | 80 | 1.25 |
| 13 | 101 | BNL | 409 | 4.05 |
| 14 | 111 | BNL | 173 | 1.56 |
| 15 | 57 | BNL | 63 | 1.11 |
| 16 | 8 | BNL | 10 | 1.25 |
| 17 | 115 | BNL | 295 | 2.57 |
| 18 | 57 | - | 57 | 1.00 |
| 19 | 123 | BNL BNL | 827 | 6.72 |
| 20 | 115 | BNL | 295 | 2.57 |

**The ratio ranges from 1.00 to 11.13, median 2.02.**
- **Single-table queries (1 to 3, 18) gain nothing:** there is one way to scan a table.
- **The large gains are the multi-way joins.** Query 9 (11×) joins four tables. The optimiser joins
  Proj to the 10 Ottawa departments first, then the 1875 filtered Works rows, and Emp last, so
  every intermediate result fits in one block. Written order starts with Emp ⋈ Works, 5000 × 15000 rows
  of nested loop, and filters only at the top.
- **Group-by queries (6, 8, 11, 13):** the group_by hashes its input in memory, where the unoptimised
  plan sorts it externally.
- **The rest** is projection pushdown making the nested loop's blocks hold more tuples.

All the joins chosen at B = 16 are nested loops. With 13 or 14 frames of block (B − 3 for a join below
another join, B − 2 at the top), the filtered and projected outer sides here are a page or two, so one
pass over the inner is the cheapest possible join. At B = 4 the same workload chooses hash joins in four
queries: 9 (the middle of its three joins), 10 (both joins), 19 (the upper of its two) and 20. No query
chooses a sort-merge join at any of B = 4, 8 or 16. The test `changing B changes the join algorithm`
checks one query whose algorithm changes with B.

## 3 Which rule mattered

Total actual I/O of the twenty queries with one rule turned off (`opt plan --without-rules N`):

| rule turned off | total I/O | against all rules on |
|---|---|---|
| none | 1598 | 1.00 |
| 1 split conjunctions | 2062 | 1.29 |
| 2 push selects below joins | 3134 | 1.96 |
| 3 cross plus equality becomes a join | 1598 | 1.00 |
| 4 push selects below rename and union | 1598 | 1.00 |
| 5 push selects below group_by | 1598 | 1.00 |
| 6 push projections down | 1916 | 1.20 |
| 7 tidy projections and selects | 1598 | 1.00 |

- **Rule 2** is worth the most: without it, filters on Emp and Works stay above the joins, so nested
  loops carry 5000 and 15000 row inputs instead of a few hundred.
- **Rule 1** matters because without it a WHERE that mixes a join predicate and a filter cannot be
  split, so neither part moves.
- **Rule 6** makes the outer blocks hold more tuples: Emp's 41 byte rows become 12 byte (eid, name)
  rows.

**Four rules are worth nothing on this workload, each for a reason**:
- **Rule 3** fires on a WHERE equality between a column of each side of a join: on a comma join it
  turns the cross product into a join, and on a JOIN ON it adds the equality to the ON condition, as
  in `SELECT COUNT(*) FROM Emp e JOIN Dept d ON e.did = d.did WHERE e.city = d.city;`. In this
  workload only query 5, the one comma join, has such an equality, and there the other side is Dept,
  one page. A cross product followed by a filter reads Dept once per block of Emp, and so does
  the join. Rule 3 saves CPU (pairs compared) but no I/O here. On query 9 written with commas it would
  matter enormously, because the unoptimised four way cross product is billions of rows.
- **Rule 4** has nothing to do in this workload's subquery (query 8), because the select on `t.n` meets a
  project on the way down, and none of the seven rules moves a select below a project. On query 18's
  union the selects are already inside each branch.
- **Rule 5** would move a HAVING on a grouping column below the group_by. No query has one, and even
  then the scan below reads the same pages: it saves group_by memory, not I/O, unless the group_by spills.
- **Rule 7** is cosmetic by design.

## 4 Where it breaks: level and salary are correlated

In this data `level` determines `salary`'s range: salary is 20000 · level plus 0 to 15000. So salary
above 160000 happens only at level 8, and `level = 8 AND salary > 160000` is the same as
`salary > 160000`. The optimiser does not know that. It multiplies:

```
sel(level = 8)          = 0.123 (histogram, exact)
sel(salary > 160000)    = 0.113 (histogram, close)
sel(both), independence = 0.123 x 0.113 = 0.014   ->  282 of 20000 rows
actual                                             -> 2467 rows (8.7 times more)
```

The query, on a larger copy of the dataset (`perf/data.py OUT 20`: Emp 20000 rows on 219 pages, Works
60000 on 236), selecting the wide Emp columns so the filtered side is many pages:

```sql
SELECT e.name, e.city, e.age, e.did, e.mgr, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid
WHERE e.level = 8 AND e.salary > 160000;
```

**The plan it chose at B = 4, with its estimate:**

```
nested_loop_join (e.eid = w.eid)                        est_rows=889      est_cost=691
  project [e.eid, e.name, e.age, e.did, e.mgr, e.city]  est_rows=282
    filter (e.level = 8 and e.salary > 160000)          est_rows=282
      seqscan Emp                                       est_cost=219
  project [w.eid, w.hours]
    seqscan Works                                       est_cost=236
```

282 estimated rows are 3 pages (`opt dp-table` gives {e} rows=282 pages=3), two blocks of the nested
loop's 2 pages, so the optimiser expects two passes over Works: 219 + 2 × 236 = 691. **What happened**:

```
nested_loop_join tuples_out=7409 comparisons=148020000 reads=0 writes=0 block_pages=2 blocks=12
  filter tuples_out=2467 comparisons=20000 reads=0 writes=0
    seqscan Emp tuples_out=20000 comparisons=0 reads=220 writes=0
  seqscan Works tuples_out=720000 comparisons=0 reads=2844 writes=0
```

2467 rows need 12 blocks, so Works was scanned 12 times: 3064 page I/Os against the 691 estimated.

**The better plan** is what the optimiser picks when the estimate is right. Dropping the redundant
`level = 8` gives the same rows (it is implied), and `salary > 160000` alone is estimated at 2250, close to
the 2467 actual. With that estimate the nested loop costs 219 + 12 × 236, and the hash join wins:

| B | chosen with the correlated predicate | I/O | chosen when the estimate is right | I/O | bad / better |
|---|---|---|---|---|---|
| 4 | nested loop, Emp side estimated 282 | 3064 | hash join, estimated 2250 | 1456 | **2.10** |
| 6 | nested loop | 1642 | hash join | 865 | 1.90 |
| 8 | nested loop | 1168 | hash join | 867 | 1.35 |
| 12 | nested loop | 931 | hash join | 873 | 1.07 |
| 16 | nested loop | 694 | nested loop | 694 | 1.00 |
| 32 | nested loop | 457 | nested loop | 457 | 1.00 |

So the misestimate cost a factor of 2.10 at B = 4 and 1.90 at B = 6. It costs nothing from B = 16, where
the block is 14 pages and even the true 2467 rows take only a few blocks, so the nested loop is right
either way. The damage depends on memory: the same 8.7× error is harmless when B is large and doubles
the I/O when B is small. The error also grows with the number of correlated predicates multiplied, and
compounds through every join above. Here there is one join, so it shows up once.

**The assumption that failed** is independence (DESIGN_M4.md section 2): sel(p AND q) = sel(p) · sel(q).
For these two columns P(salary > 160000 | level = 8) is 1: all 2467 level 8 employees earn more than
160000, and no one else does (each of `level = 8`, `salary > 160000` and both together counts 2467 rows).
Independence used the unconditional 0.113 instead.

**The statistic that would have prevented it** is a joint statistic on the pair of columns. Any of these
would do:
- a two-dimensional histogram on (level, salary), which counts the tuples in each cell of both columns
  together;
- the number of distinct (level, salary) combinations, compared with ndv(level) · ndv(salary), which
  shows the columns are far from independent;
- a functional dependency degree in the style of PostgreSQL's `CREATE STATISTICS ... (dependencies)`,
  which records that level is determined by salary for almost all rows, so the optimiser can use
  sel(salary > 160000) alone for the conjunction.

The third is the cheapest to keep and is exactly what this case needs. Without any joint statistic, a
cheaper hedge is to assume less independence between predicates on the same table. SQL Server's newer
cardinality estimator, for example, combines conjunctive predicates with exponential backoff (the most
selective in full, the next by its square root, and so on) instead of a plain product. That would have
estimated 20000 · 0.113 · √0.123 = 790 rows here: still low, but within a factor of 3.1 instead of 8.7.

## 5 The DP table of a four relation join

Query 9, at B = 16 (also in `perf/m4/results.md`, with its explain):

```
{e}  rows=5000 pages=49  * access path  cost=54
{w}  rows=1875 pages=6  * access path  cost=59
{p}  rows=500 pages=3  * access path  cost=3
{d}  rows=10 pages=1  * access path  cost=1
{e,w}  rows=1875 pages=24    {e} outer, nested_loop_join with w inner  cost=290
{e,w}  rows=1875 pages=24    {e} outer, hash_join with w inner  cost=223
{e,w}  rows=1875 pages=24    {e} outer, sort_merge_join with w inner  cost=266
{e,w}  rows=1875 pages=24  * {w} outer, nested_loop_join with e inner  cost=113
{e,w}  rows=1875 pages=24    {w} outer, hash_join with e inner  cost=223
{e,w}  rows=1875 pages=24    {w} outer, sort_merge_join with e inner  cost=266
{e,p}  no join predicate connects it, skipped
{w,p}  rows=1875 pages=14  * {w} outer, nested_loop_join with p inner  cost=62
{w,p}  rows=1875 pages=14    {w} outer, hash_join with p inner  cost=80
{w,p}  rows=1875 pages=14    {w} outer, sort_merge_join with p inner  cost=71
{w,p}  rows=1875 pages=14    {p} outer, nested_loop_join with w inner  cost=62
{w,p}  rows=1875 pages=14    {p} outer, hash_join with w inner  cost=80
{w,p}  rows=1875 pages=14    {p} outer, sort_merge_join with w inner  cost=71
{e,d}  no join predicate connects it, skipped
{w,d}  no join predicate connects it, skipped
{p,d}  rows=100 pages=2  * {p} outer, nested_loop_join with d inner  cost=4
{p,d}  rows=100 pages=2    {p} outer, hash_join with d inner  cost=12
{p,d}  rows=100 pages=2    {p} outer, sort_merge_join with d inner  cost=8
{p,d}  rows=100 pages=2    {d} outer, nested_loop_join with p inner  cost=4
{p,d}  rows=100 pages=2    {d} outer, hash_join with p inner  cost=12
{p,d}  rows=100 pages=2    {d} outer, sort_merge_join with p inner  cost=8
{e,w,p}  rows=1875 pages=32  * {e,w} outer, nested_loop_join with p inner  cost=143
{e,w,p}  rows=1875 pages=32    {e,w} outer, hash_join with p inner  cost=194
{e,w,p}  rows=1875 pages=32    {e,w} outer, sort_merge_join with p inner  cost=215
{e,w,p}  rows=1875 pages=32    {w,p} outer, nested_loop_join with e inner  cost=184
{e,w,p}  rows=1875 pages=32    {w,p} outer, hash_join with e inner  cost=256
{e,w,p}  rows=1875 pages=32    {w,p} outer, sort_merge_join with e inner  cost=291
{e,w,d}  no join predicate connects it, skipped
{e,p,d}  no join predicate connects it, skipped
{w,p,d}  rows=375 pages=5    {w,p} outer, nested_loop_join with d inner  cost=78
{w,p,d}  rows=375 pages=5    {w,p} outer, hash_join with d inner  cost=107
{w,p,d}  rows=375 pages=5    {w,p} outer, sort_merge_join with d inner  cost=92
{w,p,d}  rows=375 pages=5  * {p,d} outer, nested_loop_join with w inner  cost=65
{w,p,d}  rows=375 pages=5    {p,d} outer, hash_join with w inner  cost=81
{w,p,d}  rows=375 pages=5    {p,d} outer, sort_merge_join with w inner  cost=73
{e,w,p,d}  rows=375 pages=9    {e,w,p} outer, nested_loop_join with d inner  cost=178
{e,w,p,d}  rows=375 pages=9    {e,w,p} outer, hash_join with d inner  cost=242
{e,w,p,d}  rows=375 pages=9    {e,w,p} outer, sort_merge_join with d inner  cost=273
{e,w,p,d}  rows=375 pages=9  * {w,p,d} outer, nested_loop_join with e inner  cost=124
{e,w,p,d}  rows=375 pages=9    {w,p,d} outer, hash_join with e inner  cost=232
{e,w,p,d}  rows=375 pages=9    {w,p,d} outer, sort_merge_join with e inner  cost=276
```

The winner is ((Proj ⋈ Dept) ⋈ Works) ⋈ Emp: join Proj to the 10 Ottawa departments, the smallest
input, first (Proj as the outer; the other order also costs 4, and a tie keeps the written order), and
join the largest table last, when 375 rows are left. Five of the eleven subsets are skipped, because
no predicate connects them (Emp joins only Works, and Dept only Proj). The final plan's explain total is
120 rather than the DP's 124, because rule 6 then pushes projections into the inputs and shrinks the
pages. Its measured I/O was 127. The model's 120 is the 117 data pages of the four tables plus 3 pages
for reading the two intermediate join results, and those 3 pages were indeed read back. What it does not
charge is writing them (3 pages, the materialisation of DESIGN_M2.md section 2) and the four directory
pages the scans read: 120 + 3 + 4 = 127.

## 6 Sources

Leis et al., *How Good Are Query Optimizers, Really?* (VLDB 2015), for q-error at every node, and for
the finding that correlations and join skew, not the cost model, produce the worst estimates, which is
what sections 1 and 4 show on a small scale. Selinger et al. (SIGMOD 1979) for the dynamic programming
of section 5. The PostgreSQL documentation on extended statistics (`CREATE STATISTICS` with
`dependencies`, `ndistinct` and `mcv`) for the statistic section 4 proposes. DESIGN_M4.md section 7
says what each changed.
