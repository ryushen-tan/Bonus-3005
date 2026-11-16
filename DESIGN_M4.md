# DESIGN_M4.md: the query optimizer

The code is in `src/optimizer.hpp` (ANALYZE, estimates, the rules, the cost model, join enumeration and
the physical plan output) and `src/opt.hpp` (the `opt` tool). The tests are in `tests/test_opt.cpp`.
They check that the worked examples of sections 2, 3 and 5.1 come out the same, and that every item of
the checklist holds. They also run 25 SQL queries through all four modules at B = 4, 8 and 32, optimised
and unoptimised, and require the same rows. The `db` shell is tested too. `tests/e2e/run.py` checks 44
queries against PostgreSQL, through the four tools chained by file and through the shell.

## 1 Statistics

`opt analyze --table R` (or `ANALYZE R` in the shell, `ANALYZE R 50` for 50 buckets) computes everything
exactly, with no sampling. Both take 1 to 999999999 buckets.

For each table it records the number of live tuples (n_tuples), the number of data pages (n_pages, not
counting directory pages) and the average encoded record length (width). For each column it records the
average encoded length, the number of distinct values (ndv), and the min and max, all counted on the
sorted column. I added the column width so that a projection that drops a wide column shrinks the page
estimates above it. It also builds a histogram for each column. The histogram is equi-depth, which means
every bucket holds about the same number of tuples. It has k buckets (`--buckets`, default 20), and each
bucket keeps its lowest and highest value, its tuple count and its distinct values.

ANALYZE scans the table once. It decodes each tuple and writes each column's value to that column's own
temporary run, through one frame per column. Each run then goes through Module 2's external sort. The
sorted stream gives ndv, min and max, and the histogram is cut on the fly: value number i (from 0) of n
goes into bucket floor(i · k / n), so bucket sizes differ by at most one. That is "cutting at every n/k-th
value". A heavy value spans several buckets instead of distorting one. While it reads the table, ANALYZE
needs one frame per column and one for the scan. After that, each column's sort needs three. So it needs
the larger of (columns + 1) and 3 frames, and it says so when B is too small (measured: 8 columns need 9
frames, and 1 or 2 columns need 3).

The statistics are stored as records of kinds 3, 4 and 5 in `catalog.db` (DESIGN_M1.md section 6): one
per table, one per column, one per histogram bucket. Values are stored as text and turned back into
values with the column's type when the catalog is read. Running ANALYZE twice gives a byte-identical
`catalog.db` (tested), because the catalog is rewritten whole from the same records in the same order.

When a table has no statistics, the catalog still knows its exact live tuple count and data pages,
because the storage engine keeps them up to date, so those are used. Every other default is listed here.
ndv = n_tuples, which assumes a key, so an equality join returns the smaller side. An equality with a
constant is 0.1. A range is 1/3. Width is 4 bytes for INT and 2 + n/2 for VARCHAR(n). A string range on
a column that has statistics but no histogram is 1/3. The test `a plan with no statistics is still a
valid plan` covers this.

## 2 Selectivity

Selectivity is the fraction of rows a predicate keeps. It is implemented in `selectivity`,
`fractionEqual` and `fractionBelow`.

| predicate | selectivity | notes |
|---|---|---|
| c = v | from the histogram: for every bucket holding v, add count(b) / ndv(b), then divide by n_tuples. Without a histogram, 1 / ndv(c). 0 when v is outside [min, max] | I improved this. The module's formula takes "the bucket holding v". Equi-depth buckets split a frequent value across several buckets, so I add up all of them. For a value in one bucket it is the module's formula |
| c < v, c <= v | the whole buckets below v, plus a linear fraction (v − lo) / (hi − lo) of the bucket holding v, over n_tuples. Without a histogram, (v − min) / (max − min) | for strings the fraction of the bucket holding v is 1/2, because there is no distance between strings. `<=` and `<` are treated the same (a continuous model), which is what makes the module's worked example come out at exactly 0.2 |
| c > v, c >= v | 1 − the above | |
| c <> v | 1 − sel(c = v) | |
| p AND q | sel(p) · sel(q) | independence assumption |
| p OR q | sel(p) + sel(q) − sel(p) · sel(q) | |
| NOT p | 1 − sel(p) | |
| R.a = S.b | 1 / max(ndv(R.a), ndv(S.b)), so rows = n(R) · n(S) / max(...) | containment: every value on the side with fewer distinct values has a match |
| any other comparison of two columns | 0.1 | the module's default for other join predicates, and I use it for a two column filter too |
| a comparison of two constants | 1 or 0, by evaluating it | |

Every node's output rows are its input rows times the selectivity, rounded up and never below 1. The
number of distinct values of every output column is capped at the node's output rows. Pages are
ceil(rows · width / 4096), where width is the sum of the output columns' widths. This is how a pushed
down projection lowers the estimated cost of everything above it. A scan's pages are the table's real
data pages.

The other cardinalities work like this. A group_by gives the product of its keys' distinct values, at
most its input, and one row when it has no keys. Distinct is the same over all its columns. Union gives
at most the sum of its inputs and at most the product of the per-column sums of distinct values.
Intersect gives at most the smaller input and at most the product of the per-column minimum ndv. Except
gives at most its left input. Union_all gives the sum. Limit gives min(n, input).

The worked example of section 2 comes out exactly: 200, 2000, 40 and 10000 rows (tested). Here is where
these estimates break. The independence assumption fails for correlated columns (REPORT_M4.md section
4). Uniformity inside a bucket fails for clustered values. Containment fails for skewed join keys, as in
query 12 of REPORT_M4.md: 30% of Works has one project, the join estimate assumes every key is equally
common, and it is 152 times too low.

## 3 Rewrite rules

The rules are applied to the logical plan in this order: rules 1 to 5 in `pushSelects`, then join
enumeration, then rule 6 in `prune` and rule 7 in `tidy`. Each can be turned off (`opt plan
--without-rules 26`), which is how REPORT_M4.md measures what each one is worth.

| # | rule | when it applies, as implemented |
|---|---|---|
| 1 | a select on `p and q` becomes one select per conjunct | always. With rule 1 off, a condition is only moved and placed as a whole, in the rules and in join enumeration |
| 2 | a select moves below a join or cross into the side that has every column it names | every column of the predicate is found on that one side. A select also moves below another select, since selects commute |
| 3 | an equality between a column of each side, sitting on a cross, turns the cross into a join. On a join, it becomes part of the join's condition | the predicate is `column = column` with one column on each side |
| 4 | a select moves below a rename, with its columns renamed through it, and into both inputs of a union or union_all, with the right input's columns matched by position | always valid, except that a select is not moved below a rename sitting directly on a scan. That rename is part of the access path, moving the select gains nothing, and the module's own example (section 3) keeps `select e.age>60` above `rename e (scan Emp)` |
| 5 | a select moves below a group_by | every column it names is a grouping column, and there is at least one. A predicate on an aggregate never moves. With no grouping columns the group_by returns one row even for an empty input, so a select that names no column (a constant, like `1 = 2`) stays above it |
| 6 | a projection is put below each input of a join, a cross and a group_by, keeping the columns its ancestors need plus those the operator itself needs | it must drop at least one column. "Needed" is worked out top down from the root's output columns, so a column that a predicate higher up still uses is never dropped |
| 7 | a projection that keeps every column in order with no alias is removed, and two adjacent selects become one | always. Merged conjuncts are sorted by their text, so the same predicates give the same plan however they were written |

The worked example of section 3 comes out exactly (test `section 3 rewrite example reproduces`), and
`opt explain --rewrites` prints the plan before and after.

Rules 3 and 7 and the join ordering sometimes merge several conditions into one. A join's condition can
hold an ON condition plus equalities from the WHERE. A filter can hold the WHERE plus a HAVING that rule
5 moved down. A condition is written into the plan in Module 2's condition language, and its parser
accepts a condition at most 2000 levels tall. So a merged condition is cut into as few conditions as
will hold its conjuncts (`conjunctions`). Whatever does not fit into a join's condition becomes a filter
straight above the join, and two adjacent selects stay apart when they do not fit into one. Every
condition from the SQL front end fits on its own (1000 comparisons inside 450 levels of nesting), so
only a merge of more than 1000 comparisons is ever cut.

## 4 Cost model

Cost is counted in page I/O. M and N are the estimated pages of a node's inputs, and B is the number of
frames. These are the module's formulas with two changes.

The first change is about inputs that are a scan with filters, projections or renames on it (I call this
an access path). Reading it once costs the scan's pages, not the pages of the filtered output. So each
formula below uses read(x) for "pages read to get x once" (the scan's cost for an access path, the
estimated pages for anything else) and pages(x) for the size of x. For unfiltered inputs the two are the
same.

The second change is F, the number of frames the operator really works in. It is B, or B − 1 below an
operator that holds memory. Module 2 writes an input that holds memory to a temporary file before the
operator above it takes its own memory (DESIGN_M2.md section 2), and that input has to leave the file's
output frame free. A sort there makes runs of B − 1 pages and merges B − 2 at a time, and a nested loop
there has a block of B − 3 pages. In a left-deep join block, every join but the top one is below another
join. A group_by with no keys holds no memory, because it aggregates its one group as the input streams
past, so its input works in the same F as the group_by. At the top of a plan F = B, and the formulas are
exactly the module's.

| operator | own cost | module formula |
|---|---|---|
| seqscan | n_pages(T) | M |
| filter, project, rename, limit, union_all | 0 | 0 |
| sort | read(x) + 2 · pages(x) · (passes − 1), passes = 1 + ceil(log(ceil(M / F)) / log(F − 1)) | 2M(passes − 1) + M |
| distinct, group_by | the cheaper of the sort or hashing. For hashing, sizes are counted as Module 2 counts them, each record with its 4 byte slot in the 4088 usable bytes of a page. G is the pages of the groups, each the keys' widths + 8 per aggregate, + 8 more for sum and avg, + the declared length of the column for min and max (2 + n for a VARCHAR(n)). R is the pages of rows(x) input tuples of just the keys and the aggregated columns. W is the F − 1 frames beside the input page. The cost is read(x) when G ≤ W, else read(x) + Σ over k = 0, 1, … while G / W^k > W of W^k · ceil(r_k · W^(k+1) / G) + 2W^(k+1) · ceil(R / W^(k+1)), with r_0 = read(x) and r_k = ceil(R / W^k): each input read until its table is full, then partitioned. With no keys, read(x) | cost of the sort or the hash partitioning (DESIGN_M2 section 6) |
| nested_loop_join | read(outer) + ceil(pages(outer) / (F − 2)) · read(inner) | M + ceil(M / (B − 2)) · N |
| sort_merge_join | sortcost(outer) + sortcost(inner) + pages(outer) + pages(inner) | sort(M) + sort(N) + M + N |
| hash_join | read(outer) + read(inner) + 2 · (pages(outer) + pages(inner)) | 3(M + N) |
| union | one sort over both inputs | |
| intersect, except | like the sort merge join | |

So a plan needs B ≥ 4. Below an operator that holds memory a sort has B − 1 frames, and it needs three
to merge. `opt plan`, `opt explain`, `opt dp-table` and the shell refuse anything smaller.

A plan's total cost is the sum of its operators' own costs. The one exception is that an access path's
scan is not added again under an operator whose formula already reads it. That is the rule the module's
DP example uses. {A,B'} costs 110, not 110 plus the two scans. {A,B',C} costs 110 + 20 + 400, which adds
{A,B'}'s total because that input is not an access path. `opt explain` prints each node's own cost, as
the module's explain example does, and the plan's total on the last line.

The cost model and Module 2 agree on every operator. They use the same formulas, with the frames Module 2
gives each operator. REPORT_M2.md measures where the executor's real I/O differs from them. The
differences are one directory page per scan, part-full partition pages in the hash join, uneven
hash-grouping partitions, the sorted runs of a merge join being written once more, and materialised
intermediate results. The uneven partitions happen when a level's partitions are expected to be just
under or just over the frames, so some split and some do not, which the formula's all-or-none levels
cannot follow (REPORT_M2 section 7).

I left those out of the cost model on purpose. They are either constant per scan, or small next to the
formula's terms at the sizes the optimizer chooses between. Including the materialisation write would
also change the module's DP example (530 would become 550). The comparison of estimated with actual I/O
in REPORT_M4.md shows how large they are.

## 5 Join enumeration

I use dynamic programming over left-deep trees, in the style of Selinger (`joinBlock`). Dynamic
programming here means I find the best plan for every small set of tables first, and build bigger sets
out of those. After rules 1 to 5, each maximal block of joins and crosses, with any selects that sit
between them, is taken apart into its leaves and its predicates. A leaf is any input that is not itself
a join: a base table with its pushed down selects, a subquery, or a group_by. Each predicate is marked
with the set of leaves its columns come from. Then:

- best[{r}] is each leaf as it is, with its estimated rows, pages and cost.
- For each subset S of size 2 up to n, for each r in S whose S − {r} has a plan, and for each algorithm,
  the cost is computed as above with best[S − {r}] as the outer (left) and leaf r as the inner (right).
  The predicates applied at that join are the ones whose leaves are all in S, that include r, and that
  include something in S − {r}. So a predicate over k leaves, like `e.did = d.did OR p.did = d.did`
  over three, is applied at the first join that has all k. Hash and sort-merge joins are only considered
  when every predicate is an equality between a column of each side. The nested loop takes any
  condition, or none. The join at the top of the block is costed with the F of where the block sits
  (section 4). Every other join is the outer input of a join, so it uses B − 1.
- Cross products: first I find the subsets that a left-deep plan can build without a cross product.
  These are the ones where some leaf r joins S − {r} by a predicate and S − {r} is such a subset itself.
  When the whole query is one of these, every other subset is skipped, because a plan without cross
  products exists. This is why the module's table says "{A,C} no predicate, skipped". Sometimes the
  query is not one of these, because two parts of it have no predicate between them or are linked only by
  a predicate over three or more leaves. Then every subset is built, and a subset uses a cross product
  only when no join with a predicate can build it. So there is always a plan, as the module's "unless no
  alternative" asks. With rule 1 off, a whole WHERE over several tables is one such predicate, and it
  waits for the join that has all of them.
- Ties go to the written order (the last relation is tried as the inner first), so a query with nothing
  to choose by keeps the order it was written in.
- The rows of a subset are computed once, as the product of its leaves' rows and of every predicate
  inside it, so they do not depend on the order the subset was built in.
- A block has at most 16 leaves, because a FROM clause may name at most 16 tables (GRAMMAR_SQL.md
  section 5). The subsets are 32-bit sets. When no predicate connects the leaves, every subset is built:
  16 leaves take 0.8 seconds and 600 MB, and each extra leaf doubles both.

`opt dp-table` prints every candidate of every subset with its cost, and marks the winner with `*`.
REPORT_M4.md has the full table for a four relation join.

The module's DP example (section 5.1) comes out the same (test `section 5.1 DP table reproduces`, with
the statistics of the example given to the catalog directly). {A} is 100, {B'} is 10 and {C} is 400.
{A,B'} is 110 with B' outer, {B',C} is 410, and {A,C} is skipped. {A,B',C} is 530 with {A,B'} outer and C
inner. The alternative row comes out at 650, not the 550 the module prints. The module computes 410 + 40
+ 1 × 100, but with B = 32 a 40 page outer needs ceil(40 / 30) = 2 blocks, so it is 410 + 40 + 2 × 100.
The winner does not change.

Why left-deep, and what it costs. Left-deep gives two things. The first is a smaller search space. For n
relations there are n! left-deep orders but (2(n − 1))! / (n − 1)! bushy trees. For six relations that
is 720 against 30240, before multiplying by three algorithms per join. The DP over left-deep trees looks
at n · 2^(n−1) subset and inner pairs, which for six relations is 192. The second is pipelining. The
inner of every join is a base relation, so it can be rescanned or partitioned directly, and the outer
can be streamed into the next join.

In this executor that second benefit is only partial. The outer of a join whose input is another join is
materialised once, because two joins never hold memory at the same time (DESIGN_M2.md section 2). What
left-deep gives up is every plan that joins two intermediate results. Take a query like (A ⋈ B) ⋈ (C ⋈ D),
where both pairs are small and A ⋈ C would be huge. A bushy tree builds both small results and joins
them, while a left-deep tree has to bring C and D in one at a time onto A ⋈ B.

## 6 What comes out

The physical plan is the JSON that Module 2 runs, with no editing (every plan in the tests is run). Each
join carries its chosen algorithm, and distinct and group_by carry their mode. Each node also has
`est_rows` and `est_cost`, which Module 2 ignores and the shell's EXPLAIN ANALYZE prints beside the
actual counts. `opt explain` and the shell's EXPLAIN print one line per node in the module's format,
indented two spaces per level, like `hash_join (R.b = S.b) est_rows=2000 est_cost=330`, and then a last
line with the plan's total, `total est_cost=...`. `opt plan --baseline` gives the unoptimised plan used
in REPORT_M4.md: nothing rewritten, joins in the order written, every join a nested loop, and every
group_by and distinct sort based.

## 7 Sources and what they changed

- Selinger et al., *Access Path Selection in a Relational Database Management System*, SIGMOD 1979. The
  dynamic programming over left-deep trees, skipping cross products until they are the only way to
  continue, is theirs. So are the default selectivity of 1/10 for an equality with no statistics and 1/3
  for a range.
- Leis et al., *How Good Are Query Optimizers, Really?*, VLDB 2015. They found that cardinality errors,
  not the cost model, are what make plans bad, and that the errors build up through joins. That is why
  I report q-error (how many times too high or too low an estimate is) at every node, not only at the
  root. It is also why I tested a correlated pair of columns, the case where their measured errors were
  largest.
- Ramakrishnan and Gehrke, chapter 15. Their equivalences for selections (cascade, commute, push through
  joins) are rules 1, 2 and 7. Their warning that a projection can only be pushed down if every later
  predicate's columns survive is the "needed" computation of rule 6.
