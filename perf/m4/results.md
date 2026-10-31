```
CREATE TABLE Dept
CREATE TABLE Emp
CREATE TABLE Proj
CREATE TABLE Works
LOAD 50 rows into Dept
ANALYZE Dept: 50 tuples, 1 pages
LOAD 5000 rows into Emp
ANALYZE Emp: 5000 tuples, 54 pages
LOAD 500 rows into Proj
ANALYZE Proj: 500 tuples, 3 pages
LOAD 15000 rows into Works
ANALYZE Works: 15000 tuples, 59 pages
```

## 1 Estimation quality and 2 does optimisation pay (B = 16)

| query | nodes | median q-error | worst q-error | worst node | optimised I/O | joins chosen | unoptimised I/O | unoptimised / optimised |
|---|---|---|---|---|---|---|---|---|
| 1 | 3 | 1.16 | 1.16 | project est 625 actual 540 | 55 | - | 55 | 1.00 |
| 2 | 4 | 1.25 | 1.51 | project est 146 actual 97 | 55 | - | 55 | 1.00 |
| 3 | 4 | 4.91 | 8.82 | project est 72 actual 635 | 55 | - | 55 | 1.00 |
| 4 | 9 | 1.00 | 1.02 | project est 2700 actual 2757 | 57 | BNL | 63 | 1.11 |
| 5 | 10 | 1.00 | 1.16 | project est 625 actual 540 | 57 | BNL | 63 | 1.11 |
| 6 | 1 | 1.00 | 1.00 | limit est 10 actual 10 | 57 | - | 153 | 2.68 |
| 7 | 9 | 1.00 | 1.15 | project est 250 actual 217 | 110 | BNL | 272 | 2.47 |
| 8 | 5 | 1.00 | 1.47 | rename est 34 actual 50 | 55 | - | 163 | 2.96 |
| 9 | 19 | 1.00 | 1.34 | project est 375 actual 279 | 127 | BNL BNL BNL | 1413 | 11.13 |
| 10 | 1 | 1.00 | 1.00 | limit est 5 actual 5 | 155 | BNL BNL | 1082 | 6.98 |
| 11 | 6 | 1.00 | 1.11 | sort est 334 actual 301 | 64 | - | 182 | 2.84 |
| 12 | 10 | 1.00 | 152.27 | project est 30 actual 4568 | 64 | BNL | 80 | 1.25 |
| 13 | 10 | 1.00 | 1.00 | sort est 40 actual 40 | 101 | BNL | 409 | 4.05 |
| 14 | 9 | 1.00 | 5.00 | distinct est 25 actual 5 | 111 | BNL | 173 | 1.56 |
| 15 | 9 | 1.00 | 8.62 | project est 13 actual 112 | 57 | BNL | 63 | 1.11 |
| 16 | 10 | 1.01 | 1.06 | group_by est 50 actual 47 | 8 | BNL | 10 | 1.25 |
| 17 | 9 | 1.00 | 8.82 | project est 72 actual 635 | 115 | BNL | 295 | 2.57 |
| 18 | 7 | 1.00 | 2.00 | union est 10 actual 5 | 57 | - | 57 | 1.00 |
| 19 | 15 | 1.04 | 3.93 | project est 1140 actual 290 | 123 | BNL BNL | 827 | 6.72 |
| 20 | 11 | 1.02 | 5.26 | project est 1000 actual 190 | 115 | BNL | 295 | 2.57 |

Across all 161 nodes (23 nodes below a LIMIT left out): median q-error 1.00, worst 152.27. Unoptimised / optimised I/O: median 2.02, range 1.00 to 11.13.

## 3 Which rule mattered (B = 16)

| rules turned off | total I/O of the twenty queries | against all rules on |
|---|---|---|
| none | 1598 | 1.00 |
| 1 | 2062 | 1.29 |
| 2 | 3134 | 1.96 |
| 3 | 1598 | 1.00 |
| 4 | 1598 | 1.00 |
| 5 | 1598 | 1.00 |
| 6 | 1916 | 1.20 |
| 7 | 1598 | 1.00 |

## The DP table of query 9, a four relation join, at B = 16

```
SELECT e.name, p.name, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid JOIN Proj p ON w.pid = p.pid JOIN Dept d ON p.did = d.did WHERE d.city = 'Ottawa' AND w.hours > 35;
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
```
project [e.name, p.name, w.hours]           est_rows=375      est_cost=0
  nested_loop_join (e.eid = w.eid)          est_rows=375      est_cost=56
    project [p.name, w.eid, w.hours]        est_rows=375      est_cost=0
      nested_loop_join (w.pid = p.pid)      est_rows=375      est_cost=60
        project [p.pid, p.name]             est_rows=100      est_cost=0
          nested_loop_join (p.did = d.did)  est_rows=100      est_cost=4
            project [p.pid, p.did, p.name]  est_rows=500      est_cost=0
              rename p                      est_rows=500      est_cost=0
                seqscan Proj                est_rows=500      est_cost=3
            project [d.did]                 est_rows=10       est_cost=0
              filter (d.city = 'Ottawa')    est_rows=10       est_cost=0
                rename d                    est_rows=50       est_cost=0
                  seqscan Dept              est_rows=50       est_cost=1
        filter (w.hours > 35)               est_rows=1875     est_cost=0
          rename w                          est_rows=15000    est_cost=0
            seqscan Works                   est_rows=15000    est_cost=59
    project [e.eid, e.name]                 est_rows=5000     est_cost=0
      rename e                              est_rows=5000     est_cost=0
        seqscan Emp                         est_rows=5000     est_cost=54
total est_cost=120
```

## 4 Where it breaks: level and salary are correlated

```
CREATE TABLE Dept
CREATE TABLE Emp
CREATE TABLE Proj
CREATE TABLE Works
LOAD 200 rows into Dept
ANALYZE Dept: 200 tuples, 2 pages
LOAD 20000 rows into Emp
ANALYZE Emp: 20000 tuples, 219 pages
LOAD 2000 rows into Proj
ANALYZE Proj: 2000 tuples, 11 pages
LOAD 60000 rows into Works
ANALYZE Works: 60000 tuples, 236 pages
```

| B | with level = 8: join, estimated Emp rows, actual, I/O | salary only: join, estimated, actual, I/O | I/O ratio |
|---|---|---|---|
| 4 | BNL, 282, 2467, 3064 | HJ, 2250, 2467, 1456 | 2.10 |
| 6 | BNL, 282, 2467, 1642 | HJ, 2250, 2467, 865 | 1.90 |
| 8 | BNL, 282, 2467, 1168 | HJ, 2250, 2467, 867 | 1.35 |
| 12 | BNL, 282, 2467, 931 | HJ, 2250, 2467, 873 | 1.07 |
| 16 | BNL, 282, 2467, 694 | BNL, 2250, 2467, 694 | 1.00 |
| 32 | BNL, 282, 2467, 457 | BNL, 2250, 2467, 457 | 1.00 |

```
SELECT e.name, e.city, e.age, e.did, e.mgr, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid WHERE e.level = 8 AND e.salary > 160000;
project [e.name, e.city, e.age, e.did, e.mgr, w.hours]    est_rows=889      est_cost=0
  nested_loop_join (e.eid = w.eid)                        est_rows=889      est_cost=691
    project [e.eid, e.name, e.age, e.did, e.mgr, e.city]  est_rows=282      est_cost=0
      filter (e.level = 8 and e.salary > 160000)          est_rows=282      est_cost=0
        rename e                                          est_rows=20000    est_cost=0
          seqscan Emp                                     est_rows=20000    est_cost=219
    project [w.eid, w.hours]                              est_rows=60000    est_cost=0
      rename w                                            est_rows=60000    est_cost=0
        seqscan Works                                     est_rows=60000    est_cost=236
total est_cost=691
```
```
project tuples_out=7409 comparisons=0 reads=0 writes=0
  nested_loop_join tuples_out=7409 comparisons=148020000 reads=0 writes=0 block_pages=2 blocks=12
    project tuples_out=2467 comparisons=0 reads=0 writes=0
      filter tuples_out=2467 comparisons=20000 reads=0 writes=0
        rename e tuples_out=20000 comparisons=0 reads=0 writes=0
          seqscan Emp tuples_out=20000 comparisons=0 reads=220 writes=0
    project tuples_out=720000 comparisons=0 reads=0 writes=0
      rename w tuples_out=720000 comparisons=0 reads=0 writes=0
        seqscan Works tuples_out=720000 comparisons=0 reads=2844 writes=0
```

```
SELECT e.name, e.city, e.age, e.did, e.mgr, w.hours FROM Emp e JOIN Works w ON e.eid = w.eid WHERE e.salary > 160000;
project [e.name, e.city, e.age, e.did, e.mgr, w.hours]    est_rows=7093     est_cost=0
  hash_join (e.eid = w.eid)                               est_rows=7093     est_cost=727
    project [e.eid, e.name, e.age, e.did, e.mgr, e.city]  est_rows=2250     est_cost=0
      filter (e.salary > 160000)                          est_rows=2250     est_cost=0
        rename e                                          est_rows=20000    est_cost=0
          seqscan Emp                                     est_rows=20000    est_cost=219
    project [w.eid, w.hours]                              est_rows=60000    est_cost=0
      rename w                                            est_rows=60000    est_cost=0
        seqscan Works                                     est_rows=60000    est_cost=236
total est_cost=727
```
```
project tuples_out=7409 comparisons=0 reads=0 writes=0
  hash_join tuples_out=7409 comparisons=49405529 reads=797 writes=202 partitions=3 pages_left/right=[7/58,9/61,8/59] overflowed=3
    project tuples_out=2467 comparisons=0 reads=0 writes=0
      filter tuples_out=2467 comparisons=20000 reads=0 writes=0
        rename e tuples_out=20000 comparisons=0 reads=0 writes=0
          seqscan Emp tuples_out=20000 comparisons=0 reads=220 writes=0
    project tuples_out=60000 comparisons=0 reads=0 writes=0
      rename w tuples_out=60000 comparisons=0 reads=0 writes=0
        seqscan Works tuples_out=60000 comparisons=0 reads=237 writes=0
```
