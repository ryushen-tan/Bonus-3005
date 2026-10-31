# DESIGN_M4.md: the query optimizer

Code: `src/optimizer.hpp` (ANALYZE, estimates, the rules, the cost model, join enumeration, physical plan
output), `src/opt.hpp` (the `opt` tool). Tests: `tests/test_opt.cpp`. These check that the worked
examples of sections 2, 3 and 5.1 reproduce, and that every item of the checklist holds. They also run
23 SQL queries through all four modules at B = 4, 8 and 32, optimised and unoptimised, and require the
same rows. `tests/e2e/run.py` checks 44 queries against PostgreSQL.

## 1 Statistics

`opt analyze --table R` (or `ANALYZE R` in the shell) computes exactly, with no sampling:

| statistic | how |
|---|---|
| n_tuples(T) | the live tuples counted by the scan |
| n_pages(T) | the table's data pages, directory pages excluded |
| width(T) | the average encoded record length |
| width(T.c) | each column's average encoded length; I added it so a projection that drops a wide column shrinks the page estimates above it |
| ndv(T.c), min, max | counted on the sorted column |
| hist(T.c) | equi-depth, k buckets (`--buckets`, default 20), each with its lowest and highest value, its tuple count and its distinct values |

ANALYZE scans the table **once**: each tuple is decoded and each column's value is written to that
column's own temporary run, through one frame per column. Each run then goes through Module 2's
external sort. The sorted stream gives ndv, min and max, and the histogram is cut on the fly: value
number i (from 0) of n goes into bucket floor(i · k / n), so bucket sizes differ by at most one. That
is "cutting at every n/k-th value", and it means a heavy value spans several buckets rather than
distorting one. ANALYZE needs one frame per column plus three, and it says so when B is too small.

The statistics are records of kinds 3, 4 and 5 in `catalog.db` (DESIGN_M1.md section 6): one per
table, one per column, one per histogram bucket. Values are stored as text and turned back into values
with the column's type when the catalog is read. Running ANALYZE twice produces a byte-identical
`catalog.db` (tested), because the catalog is rewritten whole from the same records in the same order.

**Defaults when a table has no statistics.** The catalog still knows the table's exact live tuple count
and data pages, because the storage engine maintains them, so those are used. Every other default is
documented here:
- ndv = n_tuples, which assumes a key, so an equality join returns the smaller side;
- equality with a constant = 0.1;
- a range = 1/3;
- width = 4 bytes for INT and 2 + n/2 for VARCHAR(n);
- a string range on a column with statistics but no histogram = 1/3.

The test `a plan with no statistics is still a valid plan` covers this.

## 2 Selectivity

Implemented in `selectivity`, `fractionEqual` and `fractionBelow`:

| predicate | selectivity | notes |
|---|---|---|
| c = v | from the histogram, the sum over every bucket holding v of count(b) / ndv(b), divided by n_tuples; without a histogram 1 / ndv(c); 0 when v is outside [min, max] | **improved**: the module's formula takes "the bucket holding v". Equi-depth buckets split a frequent value across several buckets, so I sum all of them. For a value in one bucket it is the module's formula |
| c < v, c <= v | whole buckets below v plus a linear fraction (v − lo) / (hi − lo) of the bucket holding v, over n_tuples; without a histogram (v − min) / (max − min) | for strings the fraction of the bucket holding v is 1/2, because there is no distance between strings. `<=` and `<` are treated alike (a continuous model), which is what makes the module's worked example come out at exactly 0.2 |
| c > v, c >= v | 1 − the above | |
| c <> v | 1 − sel(c = v) | |
| p AND q | sel(p) · sel(q) | independence |
| p OR q | sel(p) + sel(q) − sel(p) · sel(q) | |
| NOT p | 1 − sel(p) | |
| R.a = S.b | 1 / max(ndv(R.a), ndv(S.b)), so rows = n(R) · n(S) / max(...) | containment: every value of the side with fewer distinct values has a match |
| any other comparison of two columns | 0.1 | the module's default for other join predicates, used for a two column filter too |
| a comparison of two constants | 1 or 0, by evaluating it | |

Every node's output rows are its input rows times the selectivity, **rounded up and never below 1**, and
the number of distinct values of every output column is capped at the node's output rows. Pages are
ceil(rows · width / 4096), where width is the sum of the output columns' widths. That is how a pushed
down projection reduces the estimated cost of everything above it. A scan's pages are the table's real
data pages. Other cardinalities:
- a group_by gives the product of its keys' distinct values, at most its input, and one row with no keys;
- distinct is the same over all its columns;
- union gives at most the sum of its inputs and at most the product of the per-column sums of distinct values;
- intersect gives at most the smaller input and the product of per-column minimum ndv;
- except gives at most its left input;
- union_all gives the sum;
- limit gives min(n, input).

The worked example of section 2 reproduces exactly: 200, 2000, 40 and 10000 rows (test). **Where these
break**:
- **Independence** fails for correlated columns (REPORT_M4.md section 4).
- **Uniformity inside a bucket** fails for clustered values.
- **Containment** fails for skewed join keys, as in query 12 of REPORT_M4.md: 30% of Works has one
  project, the join estimate assumes every key is equally common, and it is 152 times too low.

## 3 Rewrite rules

Applied to the logical plan in this order: rules 1 to 5 in `pushSelects`, then join enumeration, then
rule 6 in `prune` and rule 7 in `tidy`. Each can be turned off (`opt plan --without-rules 26`), which
is how REPORT_M4.md measures what each is worth.

| # | rule | precondition, as implemented |
|---|---|---|
| 1 | a select on `p and q` becomes one select per conjunct | none. With rule 1 off, a condition is moved and placed only as a whole, in the rules and in join enumeration |
| 2 | a select moves below a join or cross into the side that has every column it names | every column of the predicate resolves on that one side. A select also moves below another select, since selects commute |
| 3 | an equality between a column of each side, sitting on a cross, turns the cross into a join; on a join it joins the join's condition | the predicate is `column = column` with one column on each side |
| 4 | a select moves below a rename, with its columns renamed through it, and into both inputs of a union or union_all, with the right input's columns matched by position | always valid. **Except** that a select is not moved below a rename sitting directly on a scan: that rename is part of the access path, moving it gains nothing, and the module's own example (section 3) keeps `select e.age>60` above `rename e (scan Emp)` |
| 5 | a select moves below a group_by | every column it names is a grouping column. A predicate on an aggregate never moves |
| 6 | a projection below each input of a join, a cross and a group_by, keeping the columns its ancestors need plus those the operator itself needs | it must drop at least one column. "Needed" is computed top down from the root's output columns, so a column a predicate higher up still uses is never dropped |
| 7 | a projection that keeps every column in order with no alias is removed; two adjacent selects become one | none. Merged conjuncts are sorted by their text, so the same predicates give the same plan however they were written |

The worked example of section 3 reproduces exactly (test `section 3 rewrite example reproduces`), and
`opt explain --rewrites` prints the plan before and after.

## 4 Cost model

Cost is page I/O. M and N are the estimated pages of a node's inputs, B the frames. These are the
module's formulas, with one refinement. Where an input is a scan with filters, projections or renames
on it (an **access path**), reading it once costs the scan's pages, not the pages of the filtered output.
So each formula below uses read(x) for "pages read to get x once" (the scan's cost for an access path,
the estimated pages for anything else) and pages(x) for the size of x. For unfiltered inputs the two are
equal, and the formulas are exactly the module's.

| operator | own cost | module formula |
|---|---|---|
| seqscan | n_pages(T) | M |
| filter, project, rename, limit, union_all | 0 | 0 |
| sort | read(x) + 2 · pages(x) · (passes − 1), passes = 1 + ceil(log(ceil(M / B)) / log(B − 1)) | 2M(passes − 1) + M |
| distinct, group_by | the cheaper of the sort, or hashing: read(x) when pages(x) ≤ B − 1, else read(x) + 2 · pages(x); with no keys, read(x) | cost of the sort or the hash partitioning |
| nested_loop_join | read(outer) + ceil(pages(outer) / (B − 2)) · read(inner) | M + ceil(M / (B − 2)) · N |
| sort_merge_join | sortcost(outer) + sortcost(inner) + pages(outer) + pages(inner) | sort(M) + sort(N) + M + N |
| hash_join | read(outer) + read(inner) + 2 · (pages(outer) + pages(inner)) | 3(M + N) |
| union | one sort over both inputs | |
| intersect, except | like the sort merge join | |

A plan's total cost is the sum of its operators' own costs. The exception is that an access path's scan
is not added again under an operator whose formula already reads it. That is the rule the module's DP
example uses: {A,B'} costs 110, not 110 plus the two scans; {A,B',C} costs 110 + 20 + 400, which adds
{A,B'}'s total because that input is not an access path. `opt explain` prints each node's own cost, as
the module's explain example does, and the plan's total on the last line.

**The cost model and Module 2 agree on every operator**: they are the same formulas. REPORT_M2.md
measures where the executor's real I/O departs from them:
- one directory page per scan;
- part-full partition pages in the hash join;
- the sorted runs of a merge join being written once more;
- materialised intermediate results.

I left those out of the cost model on purpose. They are either constant per scan, or small next to the
formula's terms at the sizes the optimizer chooses between. Including the materialisation write would
also change the module's DP example (530 would become 550). The comparison of estimated with actual I/O
in REPORT_M4.md shows how large they are.

## 5 Join enumeration

Dynamic programming over left-deep trees, in the style of Selinger (`joinBlock`). After rules 1 to 5,
each maximal block of joins and crosses, with any selects that sit between them, is taken apart into its
leaves and its predicates. A leaf is any input that is not itself a join: a base table with its pushed
down selects, a subquery, a group_by. Each predicate is marked with the set of leaves its columns come
from. Then:

- best[{r}] is each leaf as it is, with its estimated rows, pages and cost.
- For each subset S of size 2 up to n, for each r in S whose S − {r} has a plan, and for each
  algorithm, the cost is computed as above with best[S − {r}] as the outer (left) and leaf r as the inner
  (right). The predicates applied at that join are the ones whose leaves are all in S, that include r,
  and that include something in S − {r}. Hash and sort-merge joins are considered only when all of them
  are equalities between a column of each side. The nested loop takes any condition, or none.
- **Cross products**: a subset whose leaves the predicates do not connect is skipped when the whole
  query is connected, because then a plan without cross products exists. This is why the module's table
  says "{A,C} no predicate, skipped". When the query graph is not connected, cross products are allowed.
- Ties go to the written order (the last relation is tried as the inner first), so a query with no
  information to choose by keeps the order it was written in.
- The rows of a subset are computed once, as the product of its leaves' rows and of every predicate
  inside it, so they do not depend on the order the subset was built in.

`opt dp-table` prints every candidate of every subset with its cost, and marks the winner with `*`.
REPORT_M4.md has the full table for a four relation join.

**The module's DP example (section 5.1) reproduces** (test `section 5.1 DP table reproduces`, with the
statistics of the example given to the catalog directly). {A} 100, {B'} 10, {C} 400; {A,B'} 110 with B'
outer; {B',C} 410; {A,C} skipped; {A,B',C} 530 with {A,B'} outer and C inner. The alternative row comes
out at 650, not the 550 the module prints. The module computes 410 + 40 + 1 × 100, but with B = 32 a
40 page outer needs ceil(40 / 30) = 2 blocks, so it is 410 + 40 + 2 × 100. The winner does not change.

**Why left-deep, and what it costs.** It buys two things:
- **A smaller search space.** For n relations there are n! left-deep orders but (2(n − 1))! / (n − 1)!
  bushy trees: for six relations, 720 against 30240, before multiplying by three algorithms per join.
  The DP over left-deep trees looks at n · 2^(n−1) subset and inner pairs, which for six relations is
  192.
- **Pipelining.** The inner of every join is a base relation, so it can be rescanned or partitioned
  directly, and the outer can be streamed into the next join.

In this executor that second benefit is partial. The outer of a join whose input is another join is
materialised once, because two joins never hold memory at the same time (DESIGN_M2.md section 2). What
left-deep gives up is every plan that joins two intermediate results. For a query like (A ⋈ B) ⋈ (C ⋈ D),
where both pairs are small and A ⋈ C would be huge, a bushy tree builds both small results and joins
them, while a left-deep tree has to bring C and D in one at a time onto A ⋈ B.

## 6 What comes out

The physical plan is the JSON Module 2 runs, with no editing (every plan in the tests is run). Each join
carries its chosen algorithm, and distinct and group_by carry their mode. Each node also has `est_rows`
and `est_cost`, which Module 2 ignores and the shell's EXPLAIN ANALYZE prints beside the actual counts.
`opt plan --baseline` gives the unoptimised plan of REPORT_M4.md: nothing rewritten, joins in the order
written, every join a nested loop, every group_by and distinct sort based.

## 7 Sources and what they changed

- Selinger et al., *Access Path Selection in a Relational Database Management System*, SIGMOD 1979.
  The dynamic programming over left-deep trees, skipping cross products until they are the only way to
  continue, is theirs. So is the default selectivity of 1/10 for an equality with no statistics, and of
  1/3 for a range.
- Leis et al., *How Good Are Query Optimizers, Really?*, VLDB 2015. Their result is that cardinality
  errors, not the cost model, are what make plans bad, and that they compound through joins. That made
  q-error at every node, not only at the root, the measurement to report. It is also why I tested a
  correlated pair of columns, the case where their measured errors were largest.
- Ramakrishnan and Gehrke, chapter 15. Their equivalences for selections (cascade, commute, push
  through joins) are rules 1, 2 and 7. Their warning that a projection can only be pushed if every later
  predicate's columns survive is the "needed" computation of rule 6.
