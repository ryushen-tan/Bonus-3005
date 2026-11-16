# DESIGN_M2.md: the execution engine

The operators are in `src/exec.hpp`. Checking a plan, building the operator tree and the `exec` tool are in `src/plan.hpp`. The tests are in `tests/test_exec.cpp`. The tests run 64 plans that use every join, distinct and set operator. The Bonus 1 relational algebra engine runs these same plans too, and the results have to match at B = 4, 5, 8 and 64.

## 1 The iterator and its counters

Every operator is built on `Operator` and has `open`, `next` and `close`. These wrap the virtual functions `start`, `produce` and `finish`. The wrappers do two things. `next` counts `tuples_out`. Each wrapper also points the disk manager's `attributed` counters at the operator while the call runs (`struct Charge`). This way every page read and write is charged to the operator whose code did it, including its own temporary files. If a parent's call makes a child read a page, the read is charged to the child. The operator counts `comparisons` itself: once for each tuple a filter looks at, once for each pair a nested loop looks at, and once for each key comparison in a sort, merge or hash probe.

An operator only ever calls `next` on its children. Seqscan is the only operator that touches a table, and it does that through Module 1's `HeapScan`. Temporary files go through Module 1's disk manager (`createTemp`, `readPage` and `writePage`).

`exec trace` prints one line per node in plan order, indented two spaces per level. Each line has the node's name and detail, then `tuples_out=`, `comparisons=`, `reads=` and `writes=`, then the node's own fields (like `passes=3` or `partitions=15 pages_left/right=[...] overflowed=0`), with single spaces between them. It prints no total line, because `exec run` prints the totals and the peak frames. Both use the CLOCK buffer policy unless you pass `--policy lru`, as REPORT_M1.md recommends. The policy changes no counter here. An operator's working memory is reserved frames, and temporary files are read straight into them. So only a table that gets scanned again could ever hit in the pool. The measurements in REPORT_M2.md came out the same under both policies.

## 2 The memory budget

The rule is that working memory is frames taken out of the buffer pool. The pool has exactly B frames. When an operator needs k pages of memory, it calls `pool.reserve()` k times. This covers a run being built, the partition buffers of a hash join, or a block of outer tuples. The tuples it holds are kept decoded in a vector. But the vector is only filled until the tuples would stop fitting in those k pages under the slotted page layout (`struct Packing` does the same arithmetic as `insertRecord`). So an operator can't hold more tuples than its frames could hold. The whole plan can't hold more than B frames either, because the pool has no more to give. Asking for more than B fails with `no unpinned frame available`. `pool.peak` is the most frames pinned or reserved at any one moment. `exec run --json` reports it as `peak_frames`, and the tests check that it is at most B after every plan.

Here is how B is divided among operators. Pipelined operators hold at most the one page a scan has pinned. These are seqscan, filter, project, rename, limit, union_all, and a group_by with no keys (it has one group to keep, and it aggregates that group as the input streams past). A memory-holding operator takes whatever the pool has free when it starts, minus one frame for each page its input will pin. The memory-holding operators are sort, distinct, group_by with keys, union, intersect, except, and the three joins. Two operators never hold working memory at the same time, because of this rule:

> When an input of a memory-holding operator itself contains a memory-holding operator, that input is run to completion into a temporary file before the parent takes any memory, and the parent then reads the file one page at a time.

A group_by with no keys is also written to a file under this rule, even though it holds no memory. This way, a nested loop join with it as the inner side reads its one row again for each block. Otherwise it would aggregate the whole input again every time.

`buildPlan` marks these inputs (`materialize[i]`). The child is opened first. So anything it does inside `open` (its own sorting or partitioning, its own materialised inputs) happens while every frame is free. Then one frame is taken to write its output. Every memory-holding operator below that point is told to leave that one frame free (`parentFrames`). So at any depth of nesting the overhead is one frame, not one frame per level. Say a nested loop join has such an input on both sides. It writes its inner input first and its outer input second. The outer's reader holds a frame for the rest of the join, and the inner's reader is made last. So neither input is materialised while the other holds a frame, and each one gets the whole budget.

A union_all is the one pipelined operator that opens an input late. Its right input is opened only once the left is used up. Say the parent writes the union_all's output. By the time the right input opens, the parent's writer already holds its frame. If the right input had materialised inputs of its own, it would need two output frames at once (for example, five frames at B = 4 for a sort over a distinct). So here is what happens when a union_all's output is written for a parent and its right input holds memory. The union_all first writes that right input to its own file. It does this before it opens its left input and before the parent takes its frame. Later it reads the file in place of the right input. The result is that every plan runs at B = 4. A single operator never needs more than 4 frames, and only one runs at a time. The tests run every plan at B = 4. That includes nested loop joins with a sort, a hash distinct, a hash group_by or another join on each side. It also includes a sort, a hash group_by and a nested loop join over a union_all whose right input is a sort over a distinct, a group_by over a join, or a join over a join.

The rule has a cost. The materialised result is written once and read once more than a fully pipelined executor would need. Section 6 of REPORT_M2.md measures it. Section 6 of DESIGN_M4.md explains why the cost model leaves it out.

The rule also gives something up. In a left deep tree of nested loop joins, the textbook pipelines each join's output straight into the next join's outer block. Here, the outer side of a join that has a join below it is materialised. I picked correctness at B = 4 over that saving. Two pipelined nested loop joins need at least 5 frames (two blocks of one page, two inner pages and one outer page).

## 3 Each operator against the budget

| operator | frames it holds | where |
|---|---|---|
| seqscan | 1, the data page it is handing out | Module 1's scan copies a directory page's list of data pages and unpins it, so only one page is ever pinned |
| filter, project, rename, limit | 0 | |
| union_all | 0. The right input is read only after the left is closed, so at most one scan below it holds a page. Under a parent that writes its output, a right input that holds memory is written to a file first | section 2 |
| sort | pass 0: B − 1 reserved, plus the page its input arrives in. Merges: B. Final pass: up to B − 1 run pages | below |
| distinct, group_by (sort) | same as sort. A group_by with no keys holds 0 | |
| distinct, group_by (hash) | B − 1, first all of them for the table, then all of them as partition buffers while one group is aggregated in passing. Plus the input page | section 6 |
| nested_loop_join | B − 2 block pages, taken as the block fills, plus the outer's page and the inner's page | section 2 |
| hash_join | partition phase: B − 1 buffers plus the input page. Probe phase: the build partition's pages plus one page for reading | below |
| sort_merge_join | its two sorts, one after the other. Then 3: one page of the left run, and the current and previous page of the right run | |
| union | one sort over both inputs | |
| intersect, except | two sorts to single runs, then 2 pages | |

## 4 Page accounting for the external sort

The input is N pages and there are B frames.

Pass 0 reserves B − 1 frames. It fills them with tuples until the tuples would no longer fit on B pages. Then it sorts that run in memory and writes it out, one page at a time, through one of its own frames. A run is B pages long: the B − 1 frames it reserved plus the page the input arrives in. This is the textbook setup, where the sort reads its input into its own buffers. It is also what makes the module's worked example come out right. The pass reads N pages, which are charged to the child scan. It writes N pages (a bit more when the last page of a run is part full), and it leaves ceil(N / B) runs. If the whole input fits in one run, nothing is written. The run is sorted in memory and streamed, so the sort costs only the N reads of its input (one pass).

The merge passes take B − 1 runs at a time and merge them into one output frame. That is B − 1 reads of one page each, and one page written whenever the output page fills. Runs are grouped in order. A group of just one run is carried forward without being rewritten, which is the strategy the module calls valid. Each pass that merges reads and writes the pages of the runs it merges. The passes repeat until at most B − 1 runs are left.

The final pass merges whatever is left and streams the tuples to the parent. It reads every page once and writes nothing.

Here is the worked example (module 2.5, N = 8, B = 3). The test `2.5 sort example: N = 8, B = 3 gives 3 passes, 22 reads and 14 writes` reproduces it.

| pass | what happens | reads | writes |
|---|---|---|---|
| 0 | runs of 3, 3 and 2 pages | 8 (the scan's) | 8 |
| 1 | merge runs 1 and 2 into a run of 6 pages, carry run 3 | 6 | 6 |
| 2 | merge the 6 page run and the 2 page run, streaming | 8 | 0 |
| total | 3 passes | 22 | 14 |

The trace shows the sort with 14 reads and 14 writes, and the scan with 9 reads. Those are the 8 data pages and the table's directory page.

For comparisons, I wrote my own merge sort for the in-memory runs (`mergeSort`, bottom up and stable). A merge picks the smallest head with a linear search, which costs k − 1 comparisons per tuple when k runs are live. I used my own sort instead of `std::sort` so the comparison count is the same on every standard library. That way "same data, same plan, same B, same counters" holds across machines, not just across runs on one machine.

## 5 Page accounting for the grace hash join

The left side (the build side) is M pages, the right side is N pages, and there are B frames.

In the partition phase, P = B − 1 partition buffers are used, plus the page the input arrives in. Each tuple of the left input goes to partition h1(key) mod P. When a buffer fills, it is written to the operator's temporary file, and the page numbers of each partition are remembered. Then the right input is done the same way with the same P and h1. Reads are M + N, charged to the input scans. Writes are about M + N, plus up to P part full last pages on each side.

In the probe phase, I go through each partition i. If R_i is no bigger than the frames left for it (B − 1, since one is kept for reading), I take that many frames. I read R_i into an in-memory hash table keyed by a second hash h2. Then I read S_i one page at a time and probe. The pairs of tuples with equal keys are output, left followed by right. Reads are the pages of R_i and S_i once each, so about M + N in all. A partition that is empty on either side is skipped.

Overflow happens when R_i is bigger than B − 1 pages. A skewed key causes this, and no hash function can fix it. In that case the pair (R_i, S_i) is joined by block nested loop instead. It takes blocks of R_i using every free frame, and the whole of S_i is streamed past each block. I chose this over repartitioning. Repartitioning can't split a partition whose tuples all have the same key, and that is exactly the case the skew measurement builds. The block nested loop stays inside the budget whatever the data looks like. The trace reports `overflowed=` with the number of partitions where this happened.

The total I/O when nothing overflows is (M + N) to read the inputs, about (M + N) to write the partitions, and (M + N) to read them back. That is the 3(M + N) of the formula plus the part full pages. The hash functions are FNV-1a over the bytes of the key values with a seed. I wrote this myself instead of using `std::hash`, so the partitions (and with them the counters) are the same on every platform. Numbers hash by value, so `1` and `1.0` agree. Text hashes by its 2 byte length and then its bytes, as a record holds it, so the keys ('ab', 'c') and ('a', 'bc') hash differently.

## 6 The other memory-holding operators

**sort_merge_join.** It sorts its left input to a single run on disk using all B frames, and then does the same for its right input. Then it merges with the algorithm in section 2.8 of the module. On equal keys it marks the right cursor, and for every left tuple in the group it seeks back to the mark. A run of equal keys larger than B needs no special case. The right run is already on disk, so the cursor just goes back to the mark and reads the pages again. The reader keeps the previous page in a third frame, so a group that crosses one page boundary costs no extra read. A group spread over more pages costs rereading those pages once for each matching left tuple. The trace's read count shows this.

**Hash distinct and group_by.** These work like the grace hash join, with an in-memory pass first. Let W be the number of frames free beside the input page (B − 1 at the root). All W of them are the table of groups. A tuple whose group is already in the table updates it. A new group goes in while there is room. If every group fits, that is the only pass. The input is read once and nothing is written. The first group that doesn't fit ends the table. I give the table up and read the input again from the start, with each of the W frames now the buffer of a partition. The first group of that pass is aggregated as its tuples go past, the way sort mode aggregates its current group. Every tuple of any other group goes to partition h(key) mod W. Each partition is then handled the same way, table first, with the next level's hash functions. A level that partitions finishes the group it keeps and gives each partition only the other groups. So every partition has fewer groups than its input, whatever the keys hash to, and the levels end at any B ≥ 4. A hot key can't stall it either, because in the table all its tuples fold into one entry. Sort mode sorts on the keys and aggregates adjacent equal keys.

The table and the partitions see each input tuple cut down to the columns the grouping reads. That means its keys, then the columns its aggregates take. A partition holds those tuples, not groups. A group can take more than a page (16 maxima of a VARCHAR(255) are 4244 bytes). A cut down tuple is never longer than an input tuple, so hash mode can run every grouping that sort mode can. This is also why the table is given up instead of written out. Reading the input again costs the pages read before the table filled, and it writes nothing. An entry in the table is counted as the bytes of its key, a count for each aggregate, and its sum or its minimum or maximum, plus a slot. A VARCHAR minimum or maximum is counted at its declared length because it can grow. The table holds no more entries than its W pages would hold under the slotted page layout, and an entry longer than a page takes whole pages. So what it keeps really fits in its frames. The one group of a partitioning pass is held beside them, like the current group of sort mode and the one group of a group_by with no keys. The hashes are the seeded FNV-1a from section 5, passed through murmur3's 64 bit finaliser. Without the finaliser, the low bits of FNV-1a split the keys the same way whatever the seed. With a power of two number of frames, a partition would then never split again.

For the cost, let R be the pages that the input's tuples take once they are cut down to those columns and packed with their slots, as the partitions hold them. Let G be the pages its groups take in the table. If G ≤ W, the cost is read(x). Otherwise, look at each level k = 0, 1, ... where G / W^k > W. At that level there are W^k inputs whose groups don't fit, each of r_k pages (r_0 = read(x), then r_k = ceil(R / W^k)). Each one is read until its table is full. That is ceil(r_k · W / (G / W^k)) pages if new groups come at an even rate. Then it is read again in full and split W ways. Each partition has a part full last page, and is written and read back at the next level:

cost = read(x) + Σ_k [ W^k · ceil(r_k · W^(k+1) / G) + 2 · W^(k+1) · ceil(R / W^(k+1)) ]

The trace shows `levels=` (the number of levels k), `partitions=` (W) and the pages of every partition written. Section 7 of REPORT_M2.md measures this against the formula, and the cost model of Module 4 uses the same formula.

**group_by edge cases.** With no grouping columns, empty input gives exactly one row. COUNT is 0, and SUM, AVG, MIN or MAX of it is an error, as the module says. With grouping columns, empty input gives no rows. AVG is an exact integer sum divided by the count, written with four decimals. A half is rounded away from zero, as PostgreSQL's `round` does (1/32 is 0.0313), and it is never written as -0.0000. The quotient is a double, so a half such as 34153 / 800 = 42.69125 comes out a little below or above the true half. So the rounding is done on the shortest decimal that reads back as that double, which is the half itself. Every half an AVG of INT values can be prints its exact rounding (tested for every |sum| < 3000 and count < 3300). A value that is not a half but is closer to one than a double can tell apart prints as its double reads. That takes an average near 10^9 over hundreds of thousands of rows. The double itself keeps every bit, in temporary files too. So a SUM or AVG over an AVG sums the unrounded values and only rounds what it prints. SUM is a 64 bit integer, written to temporary files as 8 bytes so a sum above 2^31 survives a sort. SUM and AVG take any number. Over a REAL (an AVG from a subquery), the sum is kept as a double and SUM is a REAL.

**union, intersect and except.** Union sorts both inputs together and drops adjacent duplicates. Intersect and except sort each input to one run and walk them together, outputting each tuple at most once (set semantics, tested with duplicates on both sides). A column that is INT on one side and REAL (an AVG) on the other is REAL. Its INT values take the REAL form as they arrive, so the column prints with four decimals whether or not its values went through a temporary file. union_all does the same.

**A tuple of a temporary file** must fit on one page with its slot, which is 4084 bytes. The tuples written are an operator's own input tuples, or the output of an input it materialises. So a longer tuple only comes from an operator whose output is wider than its input. Examples are a join of very wide tables, a projection that repeats wide columns, or a group_by of many wide minima and maxima (16 maxima of a VARCHAR(255) and a key are 4116 bytes). An operator that has to write such a tuple stops with `error: a tuple of 4085 bytes does not fit on a page of a temporary file`, and never drops it. This covers a sort, a grouping with keys or a set operation over it, or a parent that materialises it. A group_by with no keys counts it without writing it.

**limit** stops calling its child after n tuples. The test checks that the scan under `limit 2` read one page.

## 7 Plan checking and errors

`validatePlan` checks the whole shape before anything is looked up. Every node must be an object with a string `op` that names one of the 15 operators. Every required field must be there, and each field must have the right type and values (`mode` is "sort" or "hash", `n` is a non-negative integer, and `keys` and `aggregates` are the right arrays). Each operator takes exactly its own inputs: `child` for the unary ones, `left` and `right` for the binary ones, and none for seqscan. Any other one of those three fields is an error, so a stray `child` on an `except` is not quietly made into one of its inputs. Other extra fields are allowed, so the optimizer can add `est_rows` and `est_cost`. The messages name the node and its path: `Node 'sort' (root.child): missing required field 'keys'.`, `Node 'merge_join' (root.child): unknown operator 'merge_join'.` and `Node 'except' (root): unexpected field 'child'.`. The JSON reader reports malformed JSON with its line and column. The reader recurses once per array or object. So it stops at any nesting deeper than the deepest plan a statement within the SQL limits can give (`MAX_JSON_DEPTH` in sqlparse.hpp). It reports the line and column instead of overflowing the stack. exec, opt, sql and the shell do their work on a thread with a 1 GB stack (`onLargeStack` in cli.hpp), so a plan of that depth runs. After that, `buildNode` resolves every table and column and parses and type checks every condition, bottom up, before anything is opened. So all of these are rejected before any page is read: an unknown table or column, an ambiguous column, a type error, a join condition a hash join can't evaluate, or two columns with the same qualified name in a join.

I added a few things to the published operator list, all with the same JSON conventions. `rename` (with `to`) is a physical operator too, because a self join needs two names for one table and a subquery in FROM needs its alias. A `nested_loop_join` without `condition` is a cross product. A `project` column may be `[column, alias]`, and the same column may be listed more than once. A `group_by` aggregate may carry its output name as a third element, as Module 3's logical plans do.

## 8 Sources and what they changed

Graefe, *Volcano: an Extensible and Parallel Query Evaluation System*, IEEE TKDE 1994. It defines the open/next/close interface. It also says that an operator's state lives in the operator, not in a scheduler. That is why every operator here is a state machine that picks up again on the next call (the merge join's group state, the hash join's partition and bucket position).

Shapiro, *Join Processing in Database Systems with Large Main Memories*, ACM TODS 1986. Its grace hash join, with partition counts worked out from the memory available, is the partition phase here. That covers the hash join and the hash distinct and group_by, which split their input as many ways as they have frames instead of keeping half of them for a table. Shapiro also points out that partition sizes assume a good hash function on keys that aren't skewed. That is why overflow is a required path with a nested loop fallback, and not something I assume away.

Appleby, *MurmurHash3* (2011). Its 64 bit finaliser (two multiplications and three shifts) is what the hash distinct and group_by put over FNV-1a. I added it after a partition of a partition at B = 5 split no further and the recursion didn't end.

Ramakrishnan and Gehrke, chapters 13 and 14. Their external sort and block nested loop formulas are the ones in the module. They count the final merge pass as a read without a write, and that is the accounting in section 4.
