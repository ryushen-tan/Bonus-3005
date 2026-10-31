# DESIGN_M2.md: the execution engine

Code: `src/exec.hpp` (the operators), `src/plan.hpp` (checking a plan, building the operator tree, and
the `exec` tool). Tests: `tests/test_exec.cpp`. 64 of the plans the tests run, every join, distinct and
set operator among them, are also evaluated by the Bonus 1 relational algebra engine, and the bags have
to agree at B = 4, 5, 8 and 64.

## 1 The iterator and its counters

Every operator derives from `Operator` and has `open`, `next` and `close`, which wrap the virtual
`start`, `produce` and `finish`. The wrappers do two things. `next` counts `tuples_out`. Each wrapper
also points the disk manager's `attributed` counters at the operator for the duration of the call
(`struct Charge`), so every page read and write is charged to the operator whose code made it, its own
temporary files included, and a child's I/O is charged to the child even when the parent's call caused
it. `comparisons` is incremented by the operator itself: once per tuple a filter examines, once per pair
a nested loop examines, once per key comparison in a sort, merge or hash probe.

An operator only ever calls its children's `next`. Seqscan is the only operator that touches a table,
through Module 1's `HeapScan`. Temporary files go through Module 1's disk manager: `createTemp`,
`readPage` and `writePage`.

## 2 The memory budget

**Rule: working memory is frames taken out of the buffer pool.** The pool has exactly B frames. An
operator that needs k pages of memory (a run being built, the partition buffers of a hash join, a block of
outer tuples) calls `pool.reserve()` k times. The tuples it holds there are kept decoded in a vector, but
the vector is filled only until the tuples would no longer pack into those k pages under the slotted page
layout (`struct Packing` does exactly the arithmetic `insertRecord` does). So an operator cannot hold
more tuples than its frames would hold, and the plan cannot hold more than B frames, because the pool has
no more to give. Reserving past B fails with `no unpinned frame available`. `pool.peak`, the most frames
pinned or reserved at any moment, is reported as `peak_frames` by `exec run --json` and checked by the
tests to be at most B after every plan.

**How B is divided among operators.** Pipelined operators (seqscan, filter, project, rename, limit,
union_all) hold at most the one page a scan has pinned. A memory-holding operator (sort, distinct,
group_by, union, intersect, except, and the three joins) takes what the pool has free when it starts,
less one frame for each page its input will pin. Two operators never hold working memory at the same
time, because of this rule:

> When an input of a memory-holding operator itself contains a memory-holding operator, that input is
> run to completion into a temporary file before the parent takes any memory, and the parent then reads
> the file one page at a time.

`buildPlan` marks those inputs (`materialize[i]`). The child is opened first, so whatever it does inside
`open` (its own sorting or partitioning, its own materialised inputs) happens with every frame free. Then
one frame is taken to write its output. Every memory-holding operator below that point is told to leave
that one frame free (`parentFrames`). So at any depth of nesting the overhead is one frame, never one per
level. The result is that every plan runs at B = 4: a single operator never needs more than 4 frames, and
there is only ever one running. The tests run every plan at B = 4.

What the rule costs: the materialised result is written once and read once more than a fully pipelined
executor would need. Section 7 of REPORT_M2.md measures it. Section 6 of DESIGN_M4.md explains why the
cost model leaves it out.

**What the rule gives up.** In a left deep tree of nested loop joins, the textbook pipelines each join's
output straight into the next join's outer block. Here the outer of a join that has a join below it is
materialised. I chose correctness at B = 4 over that saving: two pipelined nested loop joins need at
least 5 frames (two blocks of one page, two inner pages, one outer page).

## 3 Each operator against the budget

| operator | frames it holds | where |
|---|---|---|
| seqscan | 1, the data page it is handing out | Module 1's scan, which copies a directory page's list of data pages and unpins it, so only one page is ever pinned |
| filter, project, rename, limit | 0 | |
| union_all | 0; the right input is opened only after the left is closed, so at most one scan below it holds a page | |
| sort | pass 0: B − 1 reserved, plus the page its input arrives in; merges: B; final pass: up to B − 1 run pages | below |
| distinct, group_by (sort) | as sort | |
| distinct, group_by (hash) | B − 1: table pages and partition buffers, plus the input page | section 5 |
| nested_loop_join | B − 2 block pages, taken as the block fills, plus the outer's page and the inner's page | |
| hash_join | partition phase B − 1 buffers plus the input page; probe phase the build partition's pages plus one page for reading | below |
| sort_merge_join | its two sorts, one after the other; then 3: one page of the left run, the current and previous page of the right run | |
| union | one sort over both inputs | |
| intersect, except | two sorts to single runs, then 2 pages | |

## 4 Page accounting for the external sort

N pages of input, B frames.

- **Pass 0** reserves B − 1 frames. It fills them with tuples until they would not fit on B pages, then
  sorts that run in memory and writes it, one page at a time, through one of its own frames. A run is B
  pages: the B − 1 frames it reserved plus the page the input arrives in. That is the textbook
  arrangement, where the sort reads its input into its own buffers. It is also what makes the module's
  worked example reproduce. The pass reads N pages, which are charged to the child scan, writes N pages
  (more only when the last page of each run is part full), and leaves ceil(N / B) runs. If the whole
  input fits in one run, nothing is written: the run is sorted in memory and streamed, and the sort costs
  only the N reads of its input (one pass).
- **Merge passes** take B − 1 runs at a time into one output frame: B − 1 reads of one page each and one
  page written whenever it fills. Runs are grouped in order, and a group of one run is carried forward
  untouched rather than rewritten, the strategy the module calls valid. Each pass that merges reads and
  writes the pages of the runs it merges. Passes repeat until at most B − 1 runs are left.
- **The final pass** merges what is left and streams the tuples to the parent: it reads every page once
  and writes nothing.

Worked example (module 2.5, N = 8, B = 3), reproduced by the test
`2.5 sort example: N = 8, B = 3 gives 3 passes, 22 reads and 14 writes`:

| pass | what happens | reads | writes |
|---|---|---|---|
| 0 | runs of 3, 3 and 2 pages | 8 (the scan's) | 8 |
| 1 | merge runs 1 and 2 into a run of 6 pages, carry run 3 | 6 | 6 |
| 2 | merge the 6 page run and the 2 page run, streaming | 8 | 0 |
| total | 3 passes | 22 | 14 |

The trace shows the sort with 14 reads and 14 writes and the scan with 9 reads: the 8 data pages and the
table's directory page.

Comparisons in a sort are counted by my own merge sort for in-memory runs (`mergeSort`, bottom up and
stable) and by a linear search for the smallest head in a merge, which costs k − 1 comparisons per tuple
for k live runs. I used my own sort rather than `std::sort` so that the comparison count is the same on
every standard library. "Same data, same plan, same B, same counters" then holds across machines, not
only across runs on one machine.

## 5 Page accounting for the grace hash join

M pages on the left (the build side), N on the right, B frames.

- **Partition phase.** P = B − 1 partition buffers, plus the page the input arrives in. Each tuple of the
  left input goes to partition h1(key) mod P. A buffer that fills is written to the operator's temporary
  file, and the page numbers of each partition are remembered. Then the same is done for the right input
  with the same P and h1. Reads: M + N, charged to the input scans. Writes: about M + N, plus up to P part
  full last pages on each side.
- **Probe phase.** For each partition i, if R_i is no larger than the frames left for it (B − 1, one
  being kept for reading), take that many frames, read R_i into an in-memory hash table keyed by a second
  hash h2, then read S_i one page at a time and probe. The tuple pairs whose keys are equal are output, as
  left followed by right. Reads: the pages of R_i and S_i once each, so about M + N in all. A partition
  that is empty on either side is skipped.
- **Overflow.** If R_i is larger than B − 1 pages, which a skewed key causes and no hash function can fix,
  the pair (R_i, S_i) is joined by block nested loop instead: blocks of R_i using every free frame, the
  whole of S_i streamed past each block. I chose this over repartitioning because repartitioning cannot
  split a partition whose tuples all have the same key, which is exactly the case the skew measurement
  builds. The block nested loop is bounded by the budget whatever the data. The trace reports
  `overflowed=` with the number of partitions this happened to.

Total I/O when nothing overflows: (M + N) to read the inputs, about (M + N) to write the partitions, and
(M + N) to read them back, which is the 3(M + N) of the formula plus the part-full pages. The hash
functions are FNV-1a over the bytes of the key values with a seed (numbers hash by value, so `1` and
`1.0` agree), written here rather than `std::hash` so the partitions, and with them the counters, are the
same on every platform.

## 6 The other memory-holding operators

- **sort_merge_join** sorts its left input to a single run on disk, with all B frames, and then its right
  input. It then merges with the algorithm of section 2.8 of the module: on equal keys it marks the
  right cursor, and for every left tuple of the group it seeks back to the mark. **A run of equal keys
  larger than B** needs no special case, because the right run is already on disk and the cursor simply
  goes back to the mark and reads the pages again. The reader keeps the previous page in a third frame,
  so a group that crosses one page boundary costs no extra read. A group spread over more pages costs
  rereading those pages once per matching left tuple, which the trace's read count shows.
- **Hash distinct and group_by** split the W frames free beside the input page into P = W / 2 partition
  buffers and W − P pages for a table of groups. A tuple whose group is
  in the table updates it. A tuple whose group is new goes into the table while it has room, and to
  partition h(key) mod P once it is full. Since the table never loses a group, a group is either all in
  the table or all in one partition. Each partition is then handled the same way with a different hash
  seed, so this works for any number of groups at any B ≥ 4. Sort mode sorts on the keys and aggregates
  adjacent equal keys.
- **group_by edge cases.** With no grouping columns, empty input gives exactly one row: COUNT is 0, and
  SUM, AVG, MIN or MAX of it is an error, as the module says. With grouping columns, empty input gives
  no rows. AVG is an exact integer sum divided by the count, written with four decimals. SUM is a 64 bit
  integer, written to temporary files as 8 bytes so a sum above 2^31 survives a sort.
- **union** sorts both inputs together and drops adjacent duplicates. **intersect** and **except** sort
  each input to one run and walk them together, outputting each tuple at most once (set semantics, tested
  with duplicates on both sides).
- **limit** stops calling its child after n tuples; the test checks the scan under `limit 2` read one page.

## 7 Plan checking and errors

`validatePlan` checks the whole shape before anything is looked up. Every node must be an object with a
string `op` naming one of the 15 operators, every required field must be present, and each field must
have the right type and values (`mode` is "sort" or "hash", `n` a non-negative integer, `keys` and
`aggregates` the right arrays). Extra fields are allowed, so the optimizer can add `est_rows` and
`est_cost`. Messages name the node and its path: `Node 'sort' (root.child): missing required field
'keys'.` and `Node 'merge_join' (root.child): unknown operator 'merge_join'.`. Malformed JSON is reported
by the JSON reader with its line and column. Then `buildNode` resolves every table and column and parses
and type checks every condition, bottom up, before anything is opened. So an unknown table or column, an
ambiguous column, a type error, a join condition a hash join cannot evaluate, or two columns with the same
qualified name in a join is rejected before any page is read.

Additions to the published operator list, all with the same JSON conventions:
- `rename` (`to`) is a physical operator too, because a self join needs two names for one table and a
  subquery in FROM needs its alias.
- `nested_loop_join` without `condition` is a cross product.
- A `project` column may be `[column, alias]`, and a `group_by` aggregate may carry its output name as a
  third element, as Module 3's logical plans do.

## 8 Sources and what they changed

- Graefe, *Volcano: an Extensible and Parallel Query Evaluation System*, IEEE TKDE 1994. It defines the
  open/next/close interface and the point that an operator's state lives in the operator, not in a
  scheduler. That is why every operator here is a state machine that resumes on the next call (the
  merge join's group state, the hash join's partition and bucket position).
- Shapiro, *Join Processing in Database Systems with Large Main Memories*, ACM TODS 1986. Its grace hash
  join, with partition counts derived from the memory available, is the partition phase here. Its
  observation that partition sizes assume a good hash function on non-skewed keys is why overflow is a
  required path, with a nested loop fallback, rather than an assumption.
- Ramakrishnan and Gehrke, chapters 13 and 14. Their external sort and block nested loop formulas are
  the ones in the module. Their counting of the final merge pass as a read without a write is the
  accounting in section 4.
