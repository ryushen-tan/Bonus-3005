# Design log

One entry per working session. Each says what I was trying to do, what I tried, and what broke.

## 2026-09-16, session 1: read the assignment, decide the language

Goal: understand every requirement before writing code, and decide what the language means.
Read the assignment twice and listed the 25 cases against a first draft design. Read the scanning
and parsing chapters of Crafting Interpreters and the Wikipedia pages on EBNF, recursive descent,
maximal munch and operator precedence. Looked for the RelaX help page to copy its precedence
convention; the URL the AI gave me (`dbis-uibk.github.io/relax/help`) is a 404, the table is at
`relax.mad.uom.gr/help.htm`. Decided on RelaX's order (unary, then times/join, then intersect, then
union/minus, all left associative) so `A union B minus C` is `(A union B) minus C`.

Wrote the EBNF first (now section 1 of GRAMMAR.md), then walked it against the cases. Two
design holes came out of that walk, both from AI drafts: the first data file rule only allowed bare
strings shaped like identifiers, which contradicts section 4.1 of the assignment (`D-1` or a date
must be allowed bare), and the first plan used `std::binary_search` for intersect and minus on the
assumption that every relation is always sorted. The assumption is true but silent, and the
assignment says nested loops are expected, so intersect and minus became nested loops with our own
tuple equality. Decided the keyword rule (keywords only where the grammar expects them) and wrote
it down before touching the parser.

## 2026-09-18, session 2: tokenizer, parser, tree printer

Goal: cases 1 to 17 through `--tokens` and `--tree`. Wrote the scanner as a loop over characters
with a `col` computed from the start of the current line, one `emit` per token. Maximal munch is the
one place with lookahead: `<`, `>` and `!` look at the next character. A `-` is only the start of a
number when a digit follows, so case 4 tokenizes as `>` then `-30` and `Age > - 30` is a lexical
error at the `-`.

The parser is one method per rule with `at`, `accept`, `expect` helpers. What broke: nothing in the
tree tests, all 17 passed on the first build. One choice made while writing it: the design sketch had separate code for `select`, `project` and
`rename`, but they differ only in what is inside the brackets, so they share one `unary` method that
takes the node kind.

## 2026-09-20, session 3: the operators, and three bugs

Goal: cases 18 to 25 and the extras. Loading a file is line oriented because the assignment's rule
is line oriented (comments are lines, tuples are lines). The engine binds every attribute in a
condition to a column index once, before the loop, so the join's inner loop does no name lookups.

What broke, in order:

1. Every query that loaded `tests/data.txt` failed with `expected a value at line 12 col 16`. The
   AI's tuple scanner had `i++` in the `for` header and also stepped over the comma inside the loop,
   so the first character of every value after the first was dropped and `32` loaded as `2`. Line 12
   was the first line with a one-character value (`1`), which the skip reduced to nothing, hence
   "expected a value". Removed the header increment.
2. The next build crashed with a segmentation fault on any executed query. The AI's deduplication
   compacted rows in place with `rows[n++] = std::move(rows[i])`; when nothing has been dropped yet
   `n == i` and that is a self move assignment, which in libc++ frees the vector's own buffer before
   stealing it. Rewrote `normalize` to move survivors into a new vector.
3. With the crash gone, 27 tests failed and every join returned the whole cross product, while
   `select[zz=3](R)` returned a row instead of a name error. Every attribute was reading column 0.
   The binding function was called `bind`, and because its second argument is a `std::vector`,
   argument-dependent lookup found `std::bind` in `<functional>` and preferred its forwarding
   template; the returned bind object was thrown away and my function never ran. Proved it with a
   temporary `fprintf` that never printed. Renamed the function to `bindCond`. This one is the reason
   the tests exist: the code compiled cleanly with `-Wall -Wextra -Werror` and produced plausible
   output.

After that 82 of 82 tests passed, and running the suite twice gave byte-identical output.

## 2026-09-21, session 4: generator, instrumentation, review

Goal: a deterministic generator and a first look at the cost. `gen n m k` writes `R(a, b)` with
`a = i, b = i mod g` and `S(b, c)` with `b = j mod g, c = j`, where `g = m / k`, so every R tuple
matches exactly `k` S tuples when `k` divides `m`, and no random numbers are involved; `gen 4 4 2` gives 16 comparisons
and 8 output tuples, checked by hand. At 8000 tuples per side the join does 64,000,000 comparisons in
0.25 s, about 3.9 ns per pair, so 64000 should take about 16 s.

Ran an adversarial review of the binary against the assignment text (six independent readers, each
finding re-checked by two more). Findings and what changed are in the next entry.

## 2026-09-28, session 5: what the review found, and the numbers

The review re-ran every case and tried inputs I had not: 49 claims, 28 survived a second reader.
The ones that were real and what changed:

- Five thousand nested parentheses crashed the parser with a segmentation fault (stack overflow) and
  no message. Added a depth counter in `expr`, `cond` and `notc`; past 500 levels it is a syntax
  error with a position. Tested at 400 (parses) and 600 (error).
- A chain of 30000 `union`s took 43 s to parse: the AI's `mk(k, t, {n, inter()})` copied the whole
  left subtree into the initializer list at every fold, which is quadratic. `mk` now takes its kids
  as forwarding references and moves them in; the 20000-term chain in the tests parses in
  milliseconds.
- A 400 digit number became infinity, so three different numbers compared equal and collapsed into
  one tuple with no error. `toNum` now rejects anything that does not fit in a double.
- A directory given as the data file read as an empty file, so the query failed with "unknown
  relation" instead of an io error. `run` now checks `is_directory`.
- The values `}` and `//x` printed bare, and reloading the output lost them (one closed the relation,
  the other was a comment). The single `bare()` predicate now decides both what the loader accepts
  bare and what the printer prints bare, so output always loads back unchanged; a test feeds the
  printed output of six odd strings back in.
- A string token in a syntax error looked exactly like an identifier (`found 'or'`); it now says
  `found string 'or'`, and the tree prints literals the way the language writes them, `Str('O''Brien')`.
- Left-to-right evaluation: `Nope1 union Nope2` reported whichever operand the compiler evaluated
  first. The inputs of a binary operator are now evaluated in named locals, left first.
- Readability changes for the oral: comma-operator returns split into statements, a redundant
  `c != '-'` removed, `before`/`same` share one `compareRow`, every operator takes its node and its
  inputs the same way, `inline` on header functions, and the engine's comparator and value fetch
  renamed so they no longer share names with the parser's `cmp` and `operand` rules. The generator
  now gives every R tuple exactly k matches for any k that divides m, by giving each run of k
  consecutive S tuples one key.

Rejected claims worth remembering: the base relation is copied when a query reads it (about 60% of
the measured select time at 64000 tuples). Left as is, it is linear either way and storage is a later
project.

First full sweep: comparisons are exactly n·m at every size, 3.2 ns per pair from 4000 tuples up,
13.2 s for 64000 × 64000. The least-squares slope over all seven points is 1.83 rather than 2.00
because the 8 ms run at 1000 tuples is dominated by start-up cost; every doubling from 4000 onward
multiplies the time by 4.0. Added runs with n ≠ m and with a zero match rate to make the n·m claim and
the match-rate answer checkable, then re-measured for the report.

## 2026-09-29, session 6: second review round

Ran the review again on the fixed code, this time with a reader checking every claim in README.md and
GRAMMAR.md against the binary. 37 claims, 24 survived. What changed:

- A chain of 25000 `and` terms did not nest in the parser's sense, so the depth counter let it
  through, and evaluating the 25000-deep left-leaning tree overflowed the stack: a segmentation fault
  again. Now `mk` records the depth of every node it builds and refuses a tree deeper than 500, and
  the parser's own counter moved to the four places where a construct can contain itself. Tests at the
  boundary: 500 parentheses parse, 501 fail; a chain of 499 operators parses, 500 fail.
- `} foo` in a data file was reported as "bare value must be quoted", which named the wrong problem;
  it is now "unexpected text after '}'".
- `./ra --help` was tokenized as a query and complained about the `-`. Unknown flags are a usage error.
- The documents drifted from the code in five places: the BARE rule did not exclude tabs or the `//`
  and `}` cases, the STRING rule allowed a newline the scanner rejects, the nesting limit was stated
  as 500 where the counter allowed 499, the generator comment said "exactly k" without "when k divides
  m", and the union compatibility rule did not mention the unknown type. All fixed in the documents.
- The reviewer also pointed out that an EBNF repetition `{ op , next }` does not by itself say which
  way the operator associates; the left recursive rule does. GRAMMAR.md now gives the left recursive
  rule for each binary level and explains that the repetition form is its rewrite.
- Small readability items: `take()` and `fail()` helpers in the parser instead of ten `toks[p++]` and
  three copies of the error message, the comparison operators as a table of (text, kind) pairs instead
  of arithmetic on the enum, `gen` exiting with 2 on a usage error like `ra`.

## 2026-09-30, session 7: checking the report against measurements

Goal: make sure every number and every explanation in REPORT.md comes from a measurement. Two did not.

- The first sweep ran each query once. Its 1000-tuple join took 8.46 ms, 2.6 times the per-pair cost
  of every larger size, and that one point pulled the fitted slope down to 1.835. My draft explained it
  as fixed start-up cost. The measurements say otherwise: five reruns took 3.42 to 3.55 ms, and a
  query containing two joins took exactly twice one join, so there is no fixed cost to find. It was
  a noisy single run. `perf/run.sh` now runs every query three times and keeps the fastest. The slope
  over all seven sizes is 1.988, and every doubling step is between 1.94 and 2.02. The comparison and
  output columns were identical in both sweeps.
- The draft's million-tuple section guessed at memory bandwidth to put an upper bound on the time.
  Replaced that with things I measured: `sizeof` of a value and a row, the 16 MB level 2 cache from
  `sysctl`, and 47 MB peak resident memory for the 64000 join from `/usr/bin/time -l`. The prediction is
  now 53.4 minutes, stated as a lower bound because S stops fitting in cache.
- The README said 500 nested `not`s or unary operators parse. They do not, because the tree-depth check
  counts the levels they add. Tried the boundaries (497 `not`s parse, 498 do not; 498 nested selects
  parse, 499 do not) and rewrote the limits section to state the two rules instead of a single number.
- Two statements in this log were wrong about the tuple scanner bug in session 3. It dropped the first
  character of each value, not the second. Line 12 failed because it has a one-character value, not a
  quote. Corrected above.

# Project 2

## 2026-10-30, session 8: week one, turning the engine into a library

Goal: the seven points in section 1.3 of the Project 2 handout, so the storage engine, the executor, the SQL front end and the optimizer can all use this code.

1. Separate the layers. `engine.hpp` had the operators, the output and the loader all together, and `parser.hpp` had the tree along with the parser. Now there are four headers that can be included on their own: `lexer.hpp` (tokens), `tree.hpp` (nodes, the tree printer, conditions as text), `parser.hpp` (the grammar) and `algebra.hpp` (the operators). None of them prints to standard output or opens a file. The printers take a stream, the loader in `datafile.hpp` takes the text, and only `run.hpp` touches files.
2. The condition parser is now `parseCondition(text)`. `eval(cond, row)` evaluates a condition against one tuple, after `bindCond` has looked up the attributes and type checked the comparisons.
3. Qualified names already worked everywhere in Bonus 1 (`Rel.name` must match exactly, and an unqualified name must match one attribute). Nothing to change, except that an attribute can now belong to no relation (an aggregate output like `n`). `qualified()` prints that as just the name.
4. The tree is written out as the logical plan JSON of Module 3 (`scan`, `select`, `project`, `rename`, `join`, `cross`, `union`, `intersect`, `except`, `distinct`) by `treeToJson`, and read back by `jsonToTree`. Conditions go through as text written by `condText`. It only adds parentheses where the grammar needs them, so `a = 1 or (b = 2 or c = 3)` keeps its right-nested shape. I wrote the JSON reader myself (`json.hpp`) and it reports the line and column of a syntax error. The test round-trips fifteen queries that cover every node and every comparison operator. It also checks that writing the tree it read back gives exactly the same JSON.
5. The engine no longer owns a `std::map` of relations. It gets a `source` function that returns a relation by name. The command line passes one that reads from the data file.
6. Bags. `project` keeps duplicates, loading keeps a tuple that was written twice, and `distinct(e)` is a new operator in the grammar. `union`, `intersect` and `minus` stay set operations, which matches what SQL's `UNION`, `INTERSECT` and `EXCEPT` mean.
7. All 25 cases still pass except case 23, because its meaning changed. `project[DID](Employees)` used to give `D1, D2` with sets and now gives `D1, D1, D2`. The old answer is `distinct(project[DID](Employees))`, which I added as 23b. Among the extras, x29 (duplicates in a file used to be collapsed, now kept), x39, x40 and x48 (projections after a join now show every matching pair) changed for the same reason. 122 of 122 pass.

## 2026-10-30, session 9: Module 1, storage engine and buffer pool

Goal: the five `storage` commands, with the two worked examples reproducing exactly. I wrote the byte layouts first (`page.hpp`) as functions over a page's bytes. That way I could check the 3.2 example on a page in memory before any file existed. Then I wrote the buffer pool, with LRU as access stamps and CLOCK as the module's pseudocode line for line. After that came the heap file, with first fit over the directory.

Decisions I made while writing it:

- The scan copies a directory page's list of data page ids and then unpins it. That way it holds one frame, not two. Otherwise a hash join couldn't have B − 1 partitions plus its input at B = 4 (DESIGN_M1.md section 4).
- The catalog is itself a heap file of typed records, and it gets rewritten whole on every change.
- Operator memory will be frames reserved from the pool (`reserve` and `release`). I decided this now so Module 2 can't hold memory next to the pool.
- Every command empties the pool after reading the catalog, so its counters start from zero (`clear`).

Nothing broke in the engine. Both worked examples and the checklist passed on the first build (22 of 22). One test of mine was wrong. I meant to load a record longer than 4088 bytes, but I built one of 15 × 257 + 202 = 4057 bytes, which fits. I rewrote it with sixteen 255 byte fields (4112 bytes). For the measuring, trace 3 floods both policies and gets zero hits at B = 64, which is the point of REPORT_M1.md.

## 2026-10-30, session 10: Module 2, the execution engine

Goal: every operator, the external sort's worked example, and every plan running at B = 4. I started with the iterator base and the `Charge` wrapper, which points the disk manager's counters at the running operator. Then I wrote runs and readers for temporary files, then the external sort, then the operators. The test oracle is the Bonus 1 engine. Each physical plan is mapped back to a relational algebra tree, and the two bags must match.

What broke, in order:

1. It didn't compile. The operator structs `Project`, `Rename`, `Distinct` and `Union` had the same names as enumerators of the algebra's `Kind`, and those win in an unscoped enum. I renamed the structs.
2. The three joins agreed on the row count but not on the rows. When I hashed each output, the hash join's was different. Its probe phase read the right input's partitions with a reader built for the left input's schema, so `S.c`, a VARCHAR, was decoded as an INT. The fix is one reader per side.
3. The sort-merge join read 84 pages to join 9. Every time it rewound to the start of a group of equal keys on the right, it re-read the page it had just left. I gave the right reader a second frame that keeps the previous page, which is the "3 for the merge" in the module. Now it does 14 reads.
4. The test binary crashed with a segmentation fault and no output. AddressSanitizer showed a use after free in `BufferPool::release`. My test harness kept the operator tree alive after the Database that owns the pool. That was a bug in the test. The Database now lives in the same struct and is declared first.
5. `union` over two memory-holding inputs failed at every B with "hash grouping needs at least 3 frames, 0 are free". It only materialised its right input when it got to it, and by then its own sort had taken every frame. Now every such input is materialised before it takes memory.
6. A four-table plan from the optimizer failed at B = 4 with "nested_loop_join needs at least 3 frames, 2 are free". Nested materialisation held one output frame per level. The fix: the child is opened before its parent takes the output frame, and the memory-holding operators below are told to leave one frame free.
7. That fix then lost rows. After the output frame was taken, operators that sized their memory later still left a frame free for it, so it was counted twice. At B = 4 a hash join's overflow fallback got a block of 0 frames, and a block nested loop with no block quietly produces nothing. The oracle test caught it (399869 rows against 600610). Now the reservation is cleared once the frame is taken. A block nested loop with no frames is an error instead of an empty result.

I also changed the nested loop's block so it takes frames as it fills, not all at once, so `peak_frames` shows what was really used. The sort's in-memory runs use my own merge sort, so the comparison counts don't depend on the standard library. After all this, 19 of 19 suites pass under AddressSanitizer and UndefinedBehaviorSanitizer.

## 2026-10-30, session 11: Module 3, the SQL front end

Goal: the seven translation examples and the fifteen cases. I did the grammar first (GRAMMAR_SQL.md), then the scanner and parser, then scopes and translation. In a few places the module's sketch and PostgreSQL differ (ORDER BY and LIMIT after a set operation, optional AS, `!=`). I went with PostgreSQL there, because the integration workload is checked against it. Each one is listed in GRAMMAR_SQL.md section 4.

The seven examples printed the right shapes on the first compile. The test suite ran 47 of 50 the first time, and all three failures were my expected strings, not the compiler. I had counted the type error's column as the `age` token instead of the `>` operator. A column alias after the first projection is named by the alias. And one string position was off by one.

Something broke later. Once the optimizer included both the SQL headers and the executor's, the SQL token kind `Operator` (an unscoped enumerator) hid the executor's `Operator` class. I renamed the token kinds with a `Token` suffix.

## 2026-10-30, session 12: Module 4, the optimizer

Goal: ANALYZE, the selectivity example, the rewrite example and the DP example all reproducing, and every physical plan running in Module 2. What broke:

1. The first `opt explain` crashed. In `prune(std::move(n), outputCols(n))` it isn't specified which argument is evaluated first, and here the move happened first. The fix is to compute the columns, then move.
2. The fix didn't seem to work, because the Makefile had no `opt` target and `make opt` quietly built nothing. I added the target, and the fix had worked all along.
3. The rewrite example came out with the selects below the renames. Rule 4 moved them there. That's valid, but it gains nothing over a scan and it isn't what the module's example shows. I tightened its precondition so it doesn't apply below a rename that sits directly on a scan. The DP also broke a cost tie by putting Dept first. Ties now keep the written order.
4. The DP considered {A,C}, which the module's table skips. I added the connected-subset rule: no cross products while a plan without them exists. Then the table reproduced (110, 410, skipped, 530). The alternative row is 650, not the module's 550. The module counts 1 block where 40 pages over 30 frames are 2 (DESIGN_M4.md section 5).
5. I got "unknown table Ten" on reopening a database. Once statistics made `catalog.db` span pages, first fit put a later table's column record on an earlier page than its table record, and reading assumed file order. Now all the records are read first, then applied in order: tables, columns by position, statistics.
6. AddressSanitizer reported a use after free of a `Table`. `Database::tables` was a vector, so creating a table could move the others while something still held a reference. It's a deque now.
7. The optimizer tests hung. The unoptimised plan of a four-way comma join is a cross product of 43 billion rows. The comparison now uses the query's JOIN ON twin instead.
8. Measurement problems. Turning rule 1 off changed nothing, because the join enumeration split conjunctions by itself. With rule 1 off it now keeps conditions whole. The q-error of nodes under a LIMIT was meaningless (a sort that handed over 5 of its 2000 rows), so I left them out of the statistics. UNION was estimated at l + r, ignoring that it removes duplicates (632 estimated, 5 actual). Now it's capped by the columns' distinct values.
9. The correlated-columns experiment didn't show a bad plan at first. On the scale 5 data the misestimated plan was cheaper by luck at B = 4. A bad plan needs the underestimated side to be the outer of a nested loop against a large inner, so I used the scale 20 data with the wide Emp columns. The result was 2.10 times the I/O of the plan chosen with the right estimate at B = 4 (REPORT_M4.md section 4).

## 2026-10-30, session 13: integration and end to end checking

Goal: one `db` binary, and proof that the whole system gives the right answers. The shell is only wiring. It calls the same functions the four tools call. I set up a private PostgreSQL 17 cluster with C collation, so string order is bytewise like mine, and wrote `tests/e2e/run.py`. It runs 44 queries through `sql compile`, `opt plan` and `exec run` as separate processes, at B = 4, 8 and 32, optimised and unoptimised, and compares each result with PostgreSQL's.

The first run agreed on 252 of 258. All six disagreements were one query whose AVG I compared by rounding both sides to three decimals. My AVG is already rounded to four, so a value near a boundary rounds differently from PostgreSQL's exact numeric. When I compared numbers within 0.0001 instead, it was 258 of 258.

## 2026-10-31, session 14: three audits, and what the AI-written code got wrong

Goal: find out whether "every test passes" meant the system was right. Every suite passed (225 tests), and so did the 258 end-to-end runs against PostgreSQL. Then three independent reviews each took one part of the system, along with the module specifications and a PostgreSQL 17 instance. They tried inputs my tests never used, and built the code with AddressSanitizer and UndefinedBehaviorSanitizer. I reproduced every item below a second time before I accepted it.

AI issues in Module 1 and week one:

1. A table named `catalog` erased the catalog. The AI's `createTable` built the file name as `name + ".db"`, removed any file of that name, and created the table there. For `catalog` that file is the open catalog itself, so the next command said `unknown table R`. Found by trying reserved names.
2. Case-only names shared a file. Names are case-sensitive, but macOS's file system isn't. Creating `dept` after `Dept` removed `Dept.db`, so its rows were gone. Then loading into `dept` made a scan of `Dept` decode `dept`'s 4-byte records with `Dept`'s schema, which ASan reported as a heap overflow in `decode`. Also, `--name ../victim` overwrote a file outside the database. Found the same way.
3. Records of 4085 to 4088 bytes were "loaded" and then gone. The check allowed records up to 4088 bytes, the page's usable space, but a record also needs its 4-byte slot. The heap's `insertIntoNewPage` ignored the `NO_SPACE` it got back and still counted the tuple. So `loaded 1 rows into W` was followed by a scan of 0 rows. Through the shell, `INSERT` of a 4112-byte row printed `INSERT 1` and stored nothing. I found it by testing the boundary byte by byte. My session 9 test only tried one record well past it.
4. A missing table file hung the tool. The disk manager opened files with `O_CREAT` and returned a zero page past the end of a file. A zero page 0 reads as a directory page whose next page is itself, so `scan` followed it forever. Found by deleting `R.db` and scanning.
5. The report and the code disagreed. REPORT_M1 says "Which I would ship: CLOCK", but every entry point defaulted to LRU. And trace 2, "80/20 over 200 pages", only ever touched even page ids. The generator drew twice per access from a power-of-two LCG, and the lowest bit of that LCG alternates. So the trace had 20 hot pages, not 40. Found by counting the distinct page ids in the trace file.

Smaller ones: `scan --frames 0 --policy clock` segfaulted on an empty frame vector. A rid on a directory page, or a slot of 2^31 or more (passed through `int`), let `remove` zero a page header. Non-UTF-8 text went into VARCHARs and came out as invalid JSON. Exceptions other than `Error` aborted the tool.

AI issues in Module 2:

1. `limit` had no output schema. The AI's plan builder never set it, so every SQL query with LIMIT segfaulted the shell, including the module's example 5.4. A distinct over a limit grouped every row into one, and a union of a 1-column and a 2-column limit was accepted. The end-to-end script ran the four tools chained together and never the shell, which is why it never crashed. Found by running the example queries through `./db`.
2. A column projected twice came out empty the second time. `row.push_back(std::move(in[c]))` moved the value out, so a second use of the same column copied an empty string, and `--json` printed `[40, ]`. Found by projecting one column under two names.
3. The memory budget didn't hold everywhere. A nested loop with a memory-holding operator on both sides failed at B = 4 ("hash grouping needs at least 3 frames, 2 are free"), although DESIGN_M2 says every plan runs at B = 4. Hash grouping counted only each entry's key and 16 bytes per aggregate, but kept the whole input row, so its "2 frames" really held 278576 bytes. Found by putting blocking inputs on both sides, and by adding up the bytes actually held with a test harness.
4. Output depended on B. A union of an INT column and an AVG printed `0.0000` at B = 4 and `0` at B = 64, because only values that went through a temporary file were converted to REAL. AVG printed with `%.4f`, which rounds a tie such as 1/32 to 0.0312, where PostgreSQL's `round()` gives 0.0313. Found by running each plan at several B and comparing with PostgreSQL.

Smaller ones: a sort-mode group_by with no keys kept its temporary file after `close()`. A stray `child` field on a binary node was wired in as an input. A 20000-deep plan overflowed the JSON parser's stack. The trace padded its fields and added a `total:` line that the specification's format doesn't have.

AI issues in Module 3:

1. Valid SQL compiled into plans that couldn't run. `SELECT d.did AS budget FROM Dept d ORDER BY d.budget` passed `sql check`, then every module failed with "ambiguous attribute budget". The alias was an unqualified name, and the executor's name resolution matched it against `d.budget` as well. `GROUP BY a, a` passed and failed the same way. Found by comparing with PostgreSQL, which returns 10 rows.
2. The front end rejected SQL that PostgreSQL accepts. It rejected `SELECT COUNT(*), COUNT(*)`, `SELECT name, name`, SUM of a SUM ("t.s is BIGINT") and GROUP BY an output alias. A subquery with two columns of one name was refused too. A 1000-term OR was refused as "nesting too deep", in the Bonus 1 message format instead of the specification's. An 8000-term OR overflowed the stack, because the depth guard sits below the recursion it was meant to stop.
3. A JOIN ON could name a table from an earlier comma item. PostgreSQL rejects that. Here the optimised plan ran, and the unoptimised one failed with "unknown attribute d.did". Found by running every query optimised and with `--baseline`.

AI issues in Module 4 and the shell:

1. The optimizer segfaulted when one condition spanned three tables (`WHERE e.did = d.did OR p.did = d.did`). It did the same on any comma join of three or more tables with rule 1 off. No pair was "connected", so every candidate list was empty, and the code read the plan of a subset it had never built. Found by switching each rule off in turn.
2. The cost of hash grouping was made up at small B. The model charged one partitioning pass. The executor used B/2 partitions, which is 1 at B = 4, and spilled almost everything at each level. So `SELECT DISTINCT eid, pid, hours FROM Works` was estimated at 177 page I/Os and did 3058, while the sort plan it rejected does 414. DESIGN_M4 says "the cost model and Module 2 agree on every operator". Found with EXPLAIN ANALYZE at B = 4. The sort cost also ignored the frame a memory-holding parent keeps (estimated 4 passes, 7 in the trace).
3. Rule 5 pushed `1 = 2` below a GROUP BY with no keys. "Uses only grouping columns" is true for a condition with no columns, and a GROUP BY without keys still returns one row on empty input.
4. The shell lost input without saying so. An unterminated string swallowed its statement and every later one, and the shell exited 0. A `--` comment line before EXPLAIN made the command "not SELECT, CREATE or INSERT". Error positions counted from each statement instead of from the file. `ANALYZE T abc` aborted the shell on an uncaught `stoi` exception.

Three REPORT_M4 sentences were also contradicted by its own data: one about q15's block count, one about which join algorithms B = 4 chooses, and one about rule 3.

The same mistakes showed up in a second, independent implementation of this specification in Python: the alias that also matched a qualified name, the off-by-4 page limit, `catalog` and case-only file names, SUM of a SUM, rule 5's empty condition, AVG's rounding, INT and REAL printing differently by B, and a report recommending CLOCK while the code defaulted to LRU. So these are mistakes the AI makes, not accidents of one session.

## 2026-10-31, session 15: fixing what the audits found

Goal: fix every finding from session 14, each with a regression test written to fail on the old code, and keep every report true. I fixed the four modules separately and then merged them.

What changed, by module.

Storage:

- Table names must be identifiers. `catalog` is refused in any case, and so is a name that differs from an existing one only in case. Creating a table never removes a file. A new file is opened with `O_EXCL`.
- A record over 4084 bytes is an error at load, at insert and in a temporary file. The old constant 4088 is now `USABLE_SPACE`, and `MAX_RECORD` is 4084.
- `get` and `remove` check that the rid is on one of the table's data pages, and slots are compared unsigned.
- A missing file, a read past the end of a file, or a directory chain that links back is an error, not a hang.
- The catalog is written to a temporary file and renamed over the old one.
- `--frames 0` is one error for every tool, checked in the buffer pool.
- CLOCK is the default.
- Non-UTF-8 text is refused with its line and column, by the loader and now by the SQL scanner too. The UTF-8 check moved to `lexer.hpp`, so both use the same function.
- `perf/m1.py` draws from the LCG's high bits. Trace 2 now has 40 hot pages, and I regenerated REPORT_M1's tables and sentences from the new traces.

Execution:

- `limit` has its child's schema, and a projection copies a column that is used twice.
- A nested loop writes its inner input before the outer's reader takes a frame, so blocking inputs on both sides run at B = 4.
- Hash grouping keeps only keys and aggregate state, counted at their real size. It now partitions B − 1 ways like the Grace hash join. Its cost formula is in DESIGN_M2 section 6. The estimate is 545 and the measured value is 546, where it used to do 3058.
- Set operations convert INT values to REAL, so a column prints the same way at every B. AVG rounds a half away from zero.
- Each operator takes only its own input fields.
- JSON nesting stops at 1000 levels.
- `exec trace` has the specification's format.
- SUM and AVG of a REAL (an AVG from a subquery) keep a double sum.

SQL:

- A bare name means the column with no relation, when there is one.
- ORDER BY matches output names first, and two outputs of one name are ambiguous, as in PostgreSQL.
- Repeated output columns, and default aggregate names that are already taken, get `_2`. So does a subquery's second column of one name.
- SUM of a SUM is accepted, and so is GROUP BY an output alias. A repeated GROUP BY key counts once.
- ON sees only its own JOIN chain, and `+1` is a number.
- An AND/OR chain is one node, built in a loop. A 1000-term OR now runs, and 8000 terms give a one-line error in the specification's format instead of a stack overflow.

Optimizer and shell:

- The join enumeration always produces a plan, and a predicate over k relations is applied at the first join that has them all.
- Rule 5 needs grouping keys, and looks names up in the group_by's own output.
- The sort and nested loop costs use the frames the executor gives each operator (B − 1 below a memory holder).
- The shell skips `--` comments before EXPLAIN, ANALYZE and SET. It reports error positions in the whole input and reports an unterminated string. It checks frames and bucket counts before using them.
- Every tool reports an unexpected exception as one `internal error:` line instead of aborting.
- `tests/e2e/run.py` now also runs every query through `./db`.

What broke while merging:

1. The executor and the SQL front end each added a `MAX_DEPTH` constant, so nothing compiled. The JSON parser's is now `MAX_JSON_DEPTH`.
2. The executor's new hash cost and the optimizer's new frame budget changed the same lines. The merged `hashCost` takes the operator's frames, the same way the sort cost does.
3. A wrong result appeared only when two fixes met. With the SQL change, an aggregate named like a key (`SELECT did, COUNT(*) AS did ... HAVING COUNT(*) > 100`) was no longer ambiguous. But rule 5 matched the HAVING's `did` against the key `Emp.did`, so it pushed the condition below the grouping and returned 0 rows instead of 6. Rule 5 now looks names up in the group_by's output, where `did` is the aggregate.
4. `perf/m1.py` deleted `perf/m1/` and printed its tables, so rerunning it removed `results.md`. The README says it writes that file, so now it does, as `m2.py` and `m4.py` already did after the execution and optimizer fixes.
5. Two ad-hoc runs of the unoptimised plan of a three-table comma join, on the large measurement data, wrote temporary files of 21 GB. They build a cross product of 3·10^8 rows. That plan is meant to be naive. At the end-to-end check's scales (1 and 5) it's small enough to run.

Verification:

- Tests: 225 grew to 305 (Bonus 1 124, storage 44, execution 32, SQL 85, optimizer and shell 20). All pass, also under AddressSanitizer and UndefinedBehaviorSanitizer.
- End to end: the check against PostgreSQL 17 agrees on 390 of 390 runs at scale 1 and 390 of 390 at scale 5. That is 44 queries at B = 4, 8 and 32, optimised and unoptimised, through the tools and through the shell.
- Audit reproductions: I reran the execution audit's whole script, and the storage, SQL and shell cases, on the merged code.
- Measurements: I reran the three perf scripts. REPORT_M1 and REPORT_M2 match their output exactly. I updated REPORT_M4 from it: query 12 now does 64 page I/Os (it was 82), query 20 does 115, the median gain is 2.02 and the rule 6 ablation is 1.20.

Left as they are, on purpose:

- No upper bound on `--frames`.
- A condition of more than 1000 comparisons is refused, although PostgreSQL takes it. The optimizer's `place` recurses once per conjunct.
- `3000000000` is a type error, because INT is 4 bytes. The message now says so.
- A subquery's renamed `pid_2` can be referenced, where PostgreSQL would call both columns `pid`.
- Errors still go to standard output, as in Bonus Project 1.
- The hash join's partitions are uneven for small INT keys. The grouping's new hash finaliser would fix that too, but it would move most of REPORT_M2's and REPORT_M4's numbers.

## 2026-10-31, session 16: reviewing the fixes, and fixing them again

Goal: check that session 15's fixes held. Two reviews ran on commit c304e8c. One read the diff and reverted each fix to see whether a test failed (86 reversions). The other ran about 9,200 random queries through the shell and 2,500 through the tool chain against PostgreSQL 17 at B = 4, 7 and 32. Every finding was reproduced twice. Then the two parts were fixed separately and merged.

AI issues in session 15's fixes:

1. Hash grouping could run forever. The rewrite sent the whole table and every later tuple to partitions as soon as the table overflowed, so a level wasn't sure to finish any group. The key hash also joined VARCHAR fields with no length, so ('ab','c') and ('a','bc') hashed alike at every level. That collision was older, and it had been harmless while each round emitted its table. A hash DISTINCT of 211 such pairs grew its temporary file at about 200 MB/s. The review found it in the code and reproduced it with keys that always collide.
2. Hash GROUP BY of wide aggregates failed. The partitions held group states, so 16 MAX over a VARCHAR(255) made a 4244-byte record: "does not fit on a page". The old code, which partitioned input tuples, returned 50 rows. Found from how a partition record is built.
3. One missing table file made the whole database unusable, and then `SET frames` segfaulted. Session 15 made opening a missing file an error, and the catalog opened every table's file. The shell had always dropped its database before building the new one. With the reopen now throwing, the next statement used a null database. Found by deleting one table's file mid-session.
4. The JSON depth limit was below the SQL limits. The execution fix set 1000 levels, and the SQL fix allowed 1000 set operators and 450 nested subqueries, in the same session. The shell ran 1000 UNION ALLs, but `opt plan` refused the compiled plan. I had only tried both limits in the -O2 build on the 8 MB main stack. The sanitizer build overflowed that stack at 300 nested subqueries.
5. AVG still misrounded some halves. `std::round(d * 10000)` fixed halves that are exact in binary, such as 1/32. But 34153 / 800 = 42.69125 is a double a little below the half, so it printed 42.6912 where PostgreSQL prints 42.6913. Session 15's entry said AVG rounds a half away from zero, and that was only true for exact quotients. Found by reading the rounding code and then checking real data. 10 windows of 800 Emp rows hit it.
6. The cost model and the executor disagreed again. The executor's keyless group_by stopped holding memory, but the new `memoryBelow` treated every group_by as a holder. So a DISTINCT below `COUNT(*)` was estimated at 1534 page I/Os and did 904. Also, `hashCost` sized groups by average width over 4096-byte pages, while the executor used declared length plus slot over 4088 bytes. One plan was estimated at 9 and did 152, where sorting does 28.
7. A failed page read left a frame half evicted. It was written back and unmapped but still marked used. `readPage` now throws past the end of a file, which session 15 added, and the frame code was older than that.
8. The catalog's new atomic save set 0644 on catalog.db whatever the umask, while table files follow the umask.
9. Five fixes had no test that failed without them: rule 5 looking names up in the group_by's output (the merge bug of session 15, 0 rows instead of 6), the hash cost loop, the B − 1 frames of a lower join, the `internal error:` handlers, and the type of SUM over a REAL. A test that recomputed a formula passed whatever the optimizer did.

Older problems the reviews found:

- Some plans failed at B = 4. A sort, distinct, group_by or join over a UNION ALL whose right input holds two memory users wanted 5 frames, because `UnionAll` opened its right input only after the parent had taken its output frame. The random queries hit it in 116 shell runs and 25 tool-chain runs, all at B = 4.
- `opt explain` and EXPLAIN padded their columns, but the specification shows single spaces. Session 14 had removed the same padding from `exec trace` only.
- `(SELECT ...) ORDER BY eid` refused a FROM column, because a lone parenthesised query took the set operation path. An INT literal over 11 characters (`000000000001`) was a type error. `sql check` accepted table names the catalog refuses. Rule 3 could merge ON and WHERE into a condition longer than the limit, which was reported in Bonus 1's format at line 1, column 1. The arithmetic in the ANALYZE frames message didn't match the number it asked for.
- While fixing the depth limits, I found that a FROM clause of 32 tables crashed the optimizer, and 33 or more gave wrong counts. The join enumeration keeps its subsets in 32-bit masks.

What changed:

- Execution: Text keys hash as their length and bytes. Hash grouping gives its table all W frames. On the first group that doesn't fit, it reads its input again and splits it W ways, aggregating the first group of the pass as it goes, so every level finishes a group. Partitions hold input tuples cut down to the columns the grouping reads. A union_all under a parent that writes its output copies a right input that holds memory to its own file before it opens its left. A missing file is an error for that table only. A frame is emptied before a page is read into it. catalog.db follows the umask.
- AVG: The reviewer's version took any value within two units in the last place of a half as the half. I replaced it while merging. Now the value is rounded from the shortest decimal that reads back as the double. I checked 415,599 averages against exact fractions. Both versions got all 19,715 exact halves right. The tolerance misrounded 533 others and the shortest decimal 70. Those are all averages near 10^9 that are closer to a half than a double can tell apart (the README lists this).
- Cost: A keyless group_by holds no memory in the optimizer either. `hashCost` counts groups and partition pages the way Module 2 packs them. The Works DISTINCT is now estimated at 1092 and does 1023. The wide MAX query picks sort, estimated 27, actual 28.
- SQL, optimizer and shell: The plan depth `MAX_JSON_DEPTH` is computed from the SQL limits, and a FROM clause holds at most 16 tables. exec, opt, sql and the shell run on a thread with a 1 GB stack. The physical plan writer no longer re-serialises every subtree. INT literals are judged by value. `sql check` uses the catalog's own name rule. Rule 3 and the join enumeration split a merged condition into pieces that fit. `explain` uses single spaces. A lone parenthesised query is the query inside it. `SET frames` builds the new database before dropping the old one.

Verification:

- Tests: 305 grew to 340 (Bonus 1 124, storage 47, execution 39, SQL 99, optimizer and shell 31). All pass, also under AddressSanitizer and UndefinedBehaviorSanitizer. I checked that each new test fails with its fix reverted.
- End to end: 390 of 390 runs agree with PostgreSQL at scale 1 and 390 of 390 at scale 5.
- Random queries: I reran the review's recorded queries through the shell against PostgreSQL. That was 1346 runs at scale 1 (B = 4 and 32) and 384 at scale 5 (B = 4). No result differs and no plan is refused at B = 4. Over all six recorded sets, the refusals at B = 4 went from 116 to 0. What is left is the specification's error for SUM, AVG, MIN or MAX of an empty input inside a subquery (22 runs), and one query that sorts a 75-million-row cross product, which the check's 512 MB file cap stopped, as it stopped the review's run.
- Measurements: perf/m1.py reproduces REPORT_M1 exactly. perf/m2.py changes sections 6 and 7 of REPORT_M2, and I updated them from it (B = 16: 268 reads and 169 writes). perf/m4.py changes only the explain blocks' spacing, and no number in REPORT_M4 moved.

Left as they are, on purpose:

- Set operators keep the specification's single precedence. PostgreSQL binds INTERSECT tighter.
- The optimizer's time grows with the square of a plan's depth, so the deepest allowed statements take minutes. Pretty-printed plans of that depth are gigabytes.
- The hash grouping estimate is within 10% except near a level boundary, where uneven partitions make it up to about 30% off.
- `sql check` doesn't catch two CREATE TABLEs in one script whose names differ only in case. The shell does.

## 2026-11-15, session 17: getting ready for the hidden workload

Goal: be able to load an unseen dataset, run 25 unseen queries and report each one, on a Linux machine, with the results checked against a PostgreSQL I don't control.

What broke:

1. The build failed on Linux. The `Makefile` named `clang++`, which a stock Ubuntu image doesn't have. With `make CXX=g++`, GCC's `-Wrange-loop-construct` turned three test loops into errors, because `const std::string&` bound to a temporary made from a string literal. Three tests also expected libc++'s wording of an error: `stoll: no conversion` (libstdc++ says `stoll`) and `File exists` from `create_directories` on a file (libstdc++ reports `Not a directory`). The program's behaviour was the same on both. Found by building in Docker with GCC 12.
2. The check against PostgreSQL only agreed because of its locale. It made the cluster with `--locale=C`, which compares strings byte by byte as this engine does. Under `en_US`, PostgreSQL sorts `apple, Apple, Banana` where byte order gives `Apple, Banana, apple`, and `COUNT(*) WHERE name < 'b'` was 2 there and 3 here. The generated data never mixes case, so the 390 runs still agreed on an en-US server. Found with seven rows of mixed-case fruit.
3. `CREATE DATABASE ... LOCALE 'C'` didn't fix it on a server whose default provider is ICU. That database still sorted by language. It needs `LOCALE_PROVIDER libc`.
4. There was no way to run an unseen workload. `tests/e2e/run.py` and `perf/m4.py` were wired to the generated dataset and their own query files.

What changed:

- The `Makefile` uses the system's `c++`. The test loops copy the string. The three tests take the expected message from the same library call the program makes.
- `perf/workload.py DATA_DIR queries.sql [--frames B] [--psql ...]` loads `schema.sql` and a CSV per table, runs ANALYZE, runs each query through the shell, and writes `perf/workload/report.md`. That has the rows, page I/O, time, estimated against actual rows and q-error, with each query's EXPLAIN ANALYZE beside it, and a PostgreSQL verdict per query. A result whose only difference is the order of tied rows is marked as that, not as a failure.
- The PostgreSQL helpers moved into `perf/compare.py`, used by the workload runner, the end to end check and `perf/m4.py`. Loading PostgreSQL declares every VARCHAR column `COLLATE "C"`, so the comparison is the same on any server. The statement splitter now ignores semicolons inside string literals.

Verification:

- `make && make test` on stock Ubuntu 24.04 (its default g++, no clang) and on macOS: 340 of 340.
- The end to end check against an en-US (ICU) PostgreSQL 17: 390 of 390 at scale 1.
- A rehearsal: the 64 queries of `tests/e2e/queries.sql` and `perf/m4_queries.sql` on scale 20 data (82,200 rows) through `perf/workload.py`, against the en-US server. 64 of 64 agree at B = 32 and at B = 4. The whole run takes 10 to 13 seconds, and the slowest query 0.7 seconds. The worst root estimate is q-error 8.3 (889 estimated, 7409 actual), Emp joined to Works under `level = 8 AND salary > 160000`. Those two columns are correlated on purpose (REPORT_M4.md section 4), so independence puts the filter at 282 rows where 2467 pass.
