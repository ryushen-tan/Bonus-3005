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

## 2026-10-30, session 8: week one, the engine becomes a library

Goal: the seven points of section 1.3 of the Project 2 handout, so the storage engine, the executor,
the SQL front end and the optimizer can all reach into this code.

1. Separate the layers. `engine.hpp` held the operators, the output and the loader together, and
   `parser.hpp` held the tree with the parser. Now there are four headers that can be included on their
   own: `lexer.hpp` (tokens), `tree.hpp` (nodes, the tree printer, conditions as text), `parser.hpp`
   (the grammar) and `algebra.hpp` (the operators). None of them prints to standard output or opens a
   file: the printers take a stream, the loader in `datafile.hpp` takes the text, and only `run.hpp`
   touches files.
2. The condition parser is exposed as `parseCondition(text)`, and `eval(cond, row)` evaluates it against
   one tuple after `bindCond` has resolved the attributes and type checked the comparisons.
3. Qualified names already worked everywhere in Bonus 1 (`Rel.name` must match exactly, an unqualified
   name must match one attribute). Nothing to change except that an attribute may now belong to no
   relation (an aggregate output like `n`), which `qualified()` prints as just the name.
4. The tree is written as the logical plan JSON of Module 3 (`scan`, `select`, `project`, `rename`,
   `join`, `cross`, `union`, `intersect`, `except`, `distinct`) by `treeToJson` and read back by
   `jsonToTree`. Conditions travel as text written by `condText`, which adds parentheses only where the
   grammar needs them, so `a = 1 or (b = 2 or c = 3)` keeps its right-nested shape. The JSON reader is
   hand written (`json.hpp`) and reports the line and column of a syntax error. The test round-trips
   fifteen queries covering every node and every comparison operator, and checks that writing the
   read-back tree gives byte-identical JSON.
5. The engine no longer owns a `std::map` of relations. It is given a `source` function that returns a
   relation by name; the command line passes one backed by the data file.
6. Bags. `project` keeps duplicates, loading keeps a tuple written twice, and `distinct(e)` is a new
   operator in the grammar. `union`, `intersect` and `minus` stay set operations, matching what SQL's
   `UNION`, `INTERSECT` and `EXCEPT` mean.
7. All 25 cases still pass except case 23, whose meaning changed: `project[DID](Employees)` returned
   `D1, D2` under sets and now returns `D1, D1, D2`. The old answer is `distinct(project[DID](Employees))`,
   added as 23b. Among the extras, x29 (duplicates in a file were collapsed, now kept), x39, x40 and x48
   (projections after a join now show every matching pair) changed for the same reason. 122 of 122 pass.

## 2026-10-30, session 9: Module 1, storage engine and buffer pool

Goal: the five `storage` commands, with the two worked examples reproducing exactly. Wrote the byte
layouts first (`page.hpp`) as functions over a page's bytes, so the 3.2 example could be checked on a page
in memory before any file existed. Then the buffer pool with LRU as access stamps, and CLOCK as the module's
pseudocode line for line, then the heap file with first fit over the directory.

Decisions made while writing it:
- **The scan copies a directory page's list of data page ids and unpins it**, so it holds one frame, not
  two. Otherwise a hash join could not have B − 1 partitions plus its input at B = 4 (DESIGN_M1.md 4).
- **The catalog is itself a heap file of typed records**, rewritten whole on every change.
- **Operator memory will be frames reserved from the pool** (`reserve`/`release`), decided now so
  Module 2 cannot hold memory beside the pool.
- **Every command empties the pool after reading the catalog**, so its counters start from zero (`clear`).

What broke: nothing in the engine. Both worked examples and the checklist passed on the first build
(22 of 22). One test of my own was wrong: I meant to load a record longer than 4088 bytes but built one of
15 × 257 + 202 = 4057 bytes, which fits; rewrote it with sixteen 255 byte fields (4112 bytes). Measuring:
trace 3 floods both policies, zero hits at B = 64, which is the point of REPORT_M1.md.

## 2026-10-30, session 10: Module 2, the execution engine

Goal: every operator, the external sort's worked example, and every plan running at B = 4. Wrote the
iterator base with the `Charge` wrapper that points the disk manager's counters at the running operator,
then runs and readers for temporary files, then the external sort, then the operators. The test oracle is
the Bonus 1 engine: each physical plan is mapped back to a relational algebra tree and the two bags must
agree.

What broke, in order:

1. **It did not compile.** The operator structs `Project`, `Rename`, `Distinct` and `Union` had the same
   names as enumerators of the algebra's `Kind`, which win in an unscoped enum. Renamed the structs.
2. **The three joins agreed on the row count but not on the rows.** Hashing each output showed the hash
   join's was different. Its probe phase read the right input's partitions with a reader built for the
   left input's schema, so `S.c`, a VARCHAR, was decoded as an INT. The fix is a reader per side.
3. **The sort-merge join read 84 pages to join 9.** Every rewind to the start of a group of equal keys
   on the right re-read the page it had just left. Gave the right reader a second frame that keeps the
   previous page, which is the "3 for the merge" of the module. 14 reads.
4. **The test binary crashed with a segmentation fault and no output.** AddressSanitizer showed a use
   after free in `BufferPool::release`: my test harness kept the operator tree alive after the Database
   that owns the pool. Test bug; the Database now lives in the same struct, declared first.
5. **`union` over two memory-holding inputs failed at every B** with "hash grouping needs at least 3
   frames, 0 are free". It materialised its right input only when it reached it, by which time its own
   sort had taken every frame. Materialise every such input before taking memory.
6. **A four-table plan from the optimizer failed at B = 4** with "nested_loop_join needs at least 3
   frames, 2 are free". Nested materialisation held one output frame per level. The fix: the child is
   opened before its parent takes the output frame, and the memory-holding operators below are told to
   leave one frame free.
7. **That fix then lost rows.** After the output frame was taken, operators that sized their memory
   later still left a frame free for it, so it was counted twice. At B = 4 a hash join's overflow fallback
   got a block of 0 frames, and a block nested loop with no block quietly produces nothing. The oracle
   test caught it (399869 rows against 600610). Now the reservation is cleared once the frame is taken,
   and a block nested loop with no frames is an error rather than an empty result.

Also changed: the nested loop's block takes frames as it fills rather than all at once, so `peak_frames`
reports what was used. The sort's in-memory runs use my own merge sort, so comparison counts do not
depend on the standard library. After these, 19 of 19 suites pass under AddressSanitizer and
UndefinedBehaviorSanitizer.

## 2026-10-30, session 11: Module 3, the SQL front end

Goal: the seven translation examples and the fifteen cases. Grammar first (GRAMMAR_SQL.md), then the
scanner and parser, then scopes and translation. The decisions where the module's sketch and PostgreSQL
differ (ORDER BY and LIMIT after a set operation, optional AS, `!=`) went to PostgreSQL, because the
integration workload is checked against it; each is listed in GRAMMAR_SQL.md section 4.

The seven examples printed the right shapes on the first compile. The test suite ran 47 of 50 the first
time, and all three failures were my expected strings, not the compiler. I had counted the type error's
column as the `age` token rather than the `>` operator. A column alias after the first projection is
named by the alias. And a string position was off by one.

What broke later: once the optimizer included both the SQL headers and the executor's, the SQL token
kind `Operator` (an unscoped enumerator) hid the executor's `Operator` class. Renamed the token kinds
with a `Token` suffix.

## 2026-10-30, session 12: Module 4, the optimizer

Goal: ANALYZE, the selectivity example, the rewrite example and the DP example reproducing, and every
physical plan running in Module 2. What broke:

1. **The first `opt explain` crashed.** `prune(std::move(n), outputCols(n))` is unspecified about which
   argument is evaluated first, and here the move happened first. Compute the columns, then move.
2. **The fix did not seem to work**, because the Makefile had no `opt` target and `make opt` silently
   built nothing. Added the target; the fix had worked.
3. **The rewrite example came out with the selects below the renames.** Rule 4 moved them, which is
   valid but gains nothing over a scan and is not what the module's example shows. Refined its
   precondition: not below a rename sitting directly on a scan. The DP also broke a cost tie by putting
   Dept first; ties now keep the written order.
4. **The DP considered {A,C}**, which the module's table skips. Added the connected-subset rule: no
   cross products while a plan without them exists. The table then reproduced (110, 410, skipped, 530).
   The alternative row is 650, not the module's 550: the module counts 1 block where 40 pages over 30
   frames are 2 (DESIGN_M4.md section 5).
5. **"unknown table Ten" on reopening a database.** Once statistics made `catalog.db` span pages, first
   fit put a later table's column record on an earlier page than its table record, and reading assumed
   file order. Now all records are read, then applied in order: tables, columns by position, statistics.
6. **AddressSanitizer: a use after free of a `Table`.** `Database::tables` was a vector, so creating a
   table could move the others while something held a reference. It is a deque now.
7. **The optimizer tests hung.** The unoptimised plan of a four-way comma join is a cross product of 43
   billion rows. The comparison uses the query's JOIN ON twin instead.
8. **Measurement problems.**
   - Turning rule 1 off changed nothing, because the join enumeration split conjunctions by itself. With
     rule 1 off it now keeps conditions whole.
   - The q-error of nodes under a LIMIT was meaningless (a sort that handed over 5 of its 2000 rows), so
     they are left out of the statistics.
   - UNION was estimated at l + r, ignoring that it removes duplicates (632 estimated, 5 actual). It is
     capped by the columns' distinct values now.
9. **The correlated-columns experiment did not show a bad plan at first.** On the scale 5 data the
   misestimated plan was cheaper by luck at B = 4. A bad plan needs the underestimated side to be the
   outer of a nested loop against a large inner, so I used the scale 20 data with the wide Emp columns.
   Result: 2.10 times the I/O of the plan chosen with the right estimate at B = 4 (REPORT_M4.md section 4).

## 2026-10-30, session 13: integration and end to end checking

Goal: one `db` binary, and evidence that the whole system gives the right answers. The shell is wiring
only: it calls the same functions the four tools call. Set up a private PostgreSQL 17 cluster with C
collation, so string order is bytewise like mine, and wrote `tests/e2e/run.py`. It runs 44 queries
through `sql compile`, `opt plan` and `exec run` as separate processes, at B = 4, 8 and 32, optimised
and unoptimised, and compares each result with PostgreSQL's.

The first run agreed on 252 of 258. All six disagreements were one query whose AVG I compared by rounding
both sides to three decimals. My AVG is already rounded to four, so a value near a boundary rounds
differently from PostgreSQL's exact numeric. Comparing numbers within 0.0001 instead: 258 of 258.

## 2026-10-31, session 14: three audits, and what the AI-written code got wrong

Goal: find out whether "every test passes" meant the system was right. Every suite passed (225 tests),
and so did the 258 end-to-end runs against PostgreSQL. Three independent reviews each took one part of
the system with the module specifications and a PostgreSQL 17 instance. They tried the inputs my tests
never used, and built the code with AddressSanitizer and UndefinedBehaviorSanitizer. Every item below
was reproduced a second time before I accepted it.

AI issues in Module 1 and week one:
1. **A table named `catalog` erased the catalog.** The AI's `createTable` built the file name as
   `name + ".db"`, removed any file of that name, and created the table there. For `catalog` that file is
   the open catalog itself: the next command said `unknown table R`. Found by trying reserved names.
2. **Case-only names shared a file.** Names are case-sensitive, but macOS's file system is not.
   Creating `dept` after `Dept` removed `Dept.db`, so its rows were gone. Loading into `dept` then made
   a scan of `Dept` decode `dept`'s 4-byte records with `Dept`'s schema, which ASan reported as a heap
   overflow in `decode`. `--name ../victim` overwrote a file outside the database. Found the same way.
3. **Records of 4085 to 4088 bytes were "loaded" and then gone.** The check allowed records up to 4088
   bytes, the page's usable space, but a record also needs its 4-byte slot. The heap's
   `insertIntoNewPage` ignored the `NO_SPACE` it got back and still counted the tuple:
   `loaded 1 rows into W` followed by a scan of 0 rows. Through the shell, `INSERT` of a 4112-byte row
   printed `INSERT 1` and stored nothing. Found by testing the boundary byte by byte instead of one
   record well past it, which is all my session 9 test did.
4. **A missing table file hung the tool.** The disk manager opened files with `O_CREAT` and returned a
   zero page past the end of a file. A zero page 0 reads as a directory page whose next page is itself,
   so `scan` followed it forever. Found by deleting `R.db` and scanning.
5. **The report and the code disagreed.** REPORT_M1 says "Which I would ship: CLOCK", but every entry
   point defaulted to LRU. And trace 2, "80/20 over 200 pages", only ever touched even page ids. The
   generator drew twice per access from a power-of-two LCG, and that LCG's lowest bit alternates. So
   the trace had 20 hot pages, not 40. Found by counting the distinct page ids in the trace file.

Smaller: `scan --frames 0 --policy clock` segfaulted on an empty frame vector. A rid on a directory
page, or a slot of 2^31 or more (passed through `int`), let `remove` zero a page header. Non-UTF-8 text
went into VARCHARs and out as invalid JSON. Exceptions other than `Error` aborted the tool.

AI issues in Module 2:
1. **`limit` had no output schema.** The AI's plan builder never set it, so every SQL query with LIMIT
   segfaulted the shell, including the module's example 5.4. A distinct over a limit grouped every
   row into one, and a union of a 1-column and a 2-column limit was accepted. The end-to-end script ran
   the four tools chained together, never the shell, which is why it never crashed. Found by running the
   example queries through `./db`.
2. **A column projected twice came out empty the second time.** `row.push_back(std::move(in[c]))` moved
   the value out, so a second use of the same column copied an empty string, and `--json` printed
   `[40, ]`. Found by projecting one column under two names.
3. **The budget did not hold everywhere.** A nested loop with a memory-holding operator on both sides
   failed at B = 4 ("hash grouping needs at least 3 frames, 2 are free"), although DESIGN_M2 says every
   plan runs at B = 4. Hash grouping counted only each entry's key and 16 bytes per aggregate, but kept
   the whole input row, so its "2 frames" held 278576 bytes. Found by putting blocking inputs on both
   sides, and by summing the bytes actually held with a test harness.
4. **Output depended on B.** A union of an INT column and an AVG printed `0.0000` at B = 4 and `0` at
   B = 64, because only values that passed through a temporary file were converted to REAL. AVG printed
   with `%.4f`, which rounds a tie such as 1/32 to 0.0312, where PostgreSQL's `round()` gives 0.0313.
   Found by running each plan at several B and comparing with PostgreSQL.

Smaller: a sort-mode group_by with no keys kept its temporary file after `close()`. A stray `child`
field on a binary node was wired in as an input. A 20000-deep plan overflowed the JSON parser's stack.
The trace padded its fields and added a `total:` line the specification's format does not have.

AI issues in Module 3:
1. **Valid SQL compiled into plans that could not run.** `SELECT d.did AS budget FROM Dept d ORDER BY
   d.budget` passed `sql check`, then every module failed with "ambiguous attribute budget". The alias
   was an unqualified name, and the executor's name resolution matched it against `d.budget` as well.
   `GROUP BY a, a` passed and failed the same way. Found by comparing with PostgreSQL, which returns
   10 rows.
2. **The front end rejected SQL that PostgreSQL accepts.** It rejected `SELECT COUNT(*), COUNT(*)`,
   `SELECT name, name`, SUM of a SUM ("t.s is BIGINT") and GROUP BY an output alias. A subquery with two
   columns of one name was refused too. A 1000-term OR was refused as "nesting too deep", in the Bonus 1
   message format instead of the specification's. An 8000-term OR overflowed the stack, because the
   depth guard sits below the recursion it was meant to stop.
3. **A JOIN ON could name a table from an earlier comma item.** PostgreSQL rejects that. Here the
   optimised plan ran, and the unoptimised one failed with "unknown attribute d.did". Found by running
   every query optimised and with `--baseline`.

AI issues in Module 4 and the shell:
1. **The optimizer segfaulted when one condition spanned three tables**
   (`WHERE e.did = d.did OR p.did = d.did`). It did the same on any comma join of three or more tables
   with rule 1 off. No pair was "connected", so every candidate list was empty, and the code read the
   plan of a subset it had never built. Found by switching each rule off in turn.
2. **The cost of hash grouping was a fiction at small B.** The model charged one partitioning pass. The
   executor used B/2 partitions, which is 1 at B = 4, and spilled almost everything at each level. So
   `SELECT DISTINCT eid, pid, hours FROM Works` was estimated at 177 page I/Os and did 3058, while the
   sort plan it rejected does 414. DESIGN_M4 says "the cost model and Module 2 agree on every operator".
   Found with EXPLAIN ANALYZE at B = 4. The sort cost also ignored the frame a memory-holding parent
   keeps (estimated 4 passes, 7 in the trace).
3. **Rule 5 pushed `1 = 2` below a GROUP BY with no keys.** "Uses only grouping columns" is vacuously
   true for a condition with no columns, and a GROUP BY without keys still returns one row on empty input.
4. **The shell lost input without saying so.** An unterminated string swallowed its statement and every
   later one, and the shell exited 0. A `--` comment line before EXPLAIN made the command "not SELECT,
   CREATE or INSERT". Error positions counted from each statement instead of the file. `ANALYZE T abc`
   aborted the shell on an uncaught `stoi` exception.

Three REPORT_M4 sentences were also contradicted by its own data: one about q15's block count, one
about which join algorithms B = 4 chooses, and one about rule 3.

The same mistakes appeared in a second, independent implementation of this specification in Python:
the alias that also matched a qualified name, the off-by-4 page limit, `catalog` and case-only file
names, SUM of a SUM, rule 5's empty condition, AVG's rounding, INT and REAL printing differently by B,
and a report recommending CLOCK while the code defaulted to LRU. They are mistakes the AI makes, not
accidents of one session.

## 2026-10-31, session 15: fixing what the audits found

Goal: fix every finding of session 14, each with a regression test written to fail on the old code, and
keep every report true. The four modules were fixed separately and merged.

What changed, by module:
- **Storage.**
  - Table names must be identifiers. `catalog` is refused in any case, and so is a name that differs
    from an existing one only in case. Creating a table never removes a file: a new file is opened
    with `O_EXCL`.
  - A record over 4084 bytes is an error at load, at insert and in a temporary file. The old constant
    4088 is now `USABLE_SPACE`, and `MAX_RECORD` is 4084.
  - `get` and `remove` check the rid is on one of the table's data pages, and slots are compared
    unsigned.
  - A missing file, a read past the end of a file, or a directory chain that links back is an error,
    not a hang.
  - The catalog is written to a temporary file and renamed over the old one.
  - `--frames 0` is one error for every tool, checked in the buffer pool.
  - CLOCK is the default.
  - Non-UTF-8 text is refused with its line and column, by the loader and now by the SQL scanner too.
    The UTF-8 check moved to `lexer.hpp`, so both use the same function.
  - `perf/m1.py` draws from the LCG's high bits. Trace 2 now has 40 hot pages, and REPORT_M1's tables
    and sentences were regenerated from the new traces.
- **Execution.**
  - `limit` has its child's schema, and a projection copies a column used twice.
  - A nested loop writes its inner input before the outer's reader takes a frame, so blocking inputs on
    both sides run at B = 4.
  - Hash grouping keeps only keys and aggregate state, counted at their real size. It now partitions
    B − 1 ways like the Grace hash join. Its cost formula is in DESIGN_M2 §6: estimated 545, measured
    546, where it used to do 3058.
  - Set operations convert INT values to REAL, so a column prints one way at every B. AVG rounds a
    half away from zero.
  - Each operator takes only its own input fields.
  - JSON nesting stops at 1000 levels.
  - `exec trace` has the specification's format.
  - SUM and AVG of a REAL (an AVG from a subquery) keep a double sum.
- **SQL.**
  - A bare name means the column with no relation when there is one.
  - ORDER BY matches output names first, and two outputs of one name are ambiguous, as in PostgreSQL.
  - Repeated output columns, and default aggregate names that are already taken, get `_2`. So does a
    subquery's second column of one name.
  - SUM of a SUM is accepted, and so is GROUP BY an output alias. A repeated GROUP BY key counts once.
  - ON sees only its own JOIN chain, and `+1` is a number.
  - An AND/OR chain is one node, built in a loop. A 1000-term OR now runs, and 8000 terms are a
    one-line error in the specification's format instead of a stack overflow.
- **Optimizer and shell.**
  - The join enumeration always produces a plan, and a predicate over k relations is applied at the
    first join that has them all.
  - Rule 5 needs grouping keys, and looks names up in the group_by's own output.
  - The sort and nested loop costs use the frames the executor gives each operator (B − 1 below a
    memory holder).
  - The shell skips `--` comments before EXPLAIN, ANALYZE and SET. It reports error positions in the
    whole input and reports an unterminated string. It checks frames and bucket counts before using
    them.
  - Every tool reports an unexpected exception as one `internal error:` line instead of aborting.
  - `tests/e2e/run.py` now also runs every query through `./db`.

What broke while merging:
1. The executor and the SQL front end each added a `MAX_DEPTH` constant, so nothing compiled. The JSON
   parser's is now `MAX_JSON_DEPTH`.
2. The executor's new hash cost and the optimizer's new frame budget changed the same lines. The merged
   `hashCost` takes the operator's frames, the same way the sort cost does.
3. **A wrong result appeared only when two fixes met.** With the SQL change, an aggregate named like a
   key (`SELECT did, COUNT(*) AS did ... HAVING COUNT(*) > 100`) was no longer ambiguous. But rule 5
   matched the HAVING's `did` against the key `Emp.did`, so it pushed the condition below the grouping
   and returned 0 rows instead of 6. Rule 5 now resolves names in the group_by's output, where `did` is
   the aggregate.
4. `perf/m1.py` deleted `perf/m1/` and printed its tables, so rerunning it removed `results.md`.
   README says it writes that file, so it does now, as `m2.py` and `m4.py` already did after the
   execution and optimizer fixes.
5. Two ad-hoc runs of the unoptimised plan of a three-table comma join, on the large measurement
   data, wrote temporary files of 21 GB. They materialise a cross product of 3·10^8 rows. That plan is
   meant to be naive. At the end-to-end check's scales (1 and 5) it is small enough to run.

Verification:
- **Tests:** 225 grew to 305 (Bonus 1 124, storage 44, execution 32, SQL 85, optimizer and shell 20),
  and all pass, also under AddressSanitizer and UndefinedBehaviorSanitizer.
- **End to end:** the check against PostgreSQL 17 agrees on 390 of 390 runs at scale 1 and 390 of 390
  at scale 5. That is 44 queries at B = 4, 8 and 32, optimised and unoptimised, through the tools and
  through the shell.
- **Audit reproductions:** rerun on the merged code: the execution audit's whole script, and the
  storage, SQL and shell cases.
- **Measurements:** the three perf scripts were rerun. REPORT_M1 and REPORT_M2 match their output
  exactly. REPORT_M4 was updated from it: query 12 now does 64 page I/Os (it was 82), query 20 does
  115, the median gain is 2.02 and the rule 6 ablation is 1.20.

Left as they are, on purpose:
- No upper bound on `--frames`.
- A condition of more than 1000 comparisons is refused, although PostgreSQL takes it. The
  optimizer's `place` recurses once per conjunct.
- `3000000000` is a type error, because INT is 4 bytes; the message now says so.
- A subquery's renamed `pid_2` can be referenced, where PostgreSQL would call both columns `pid`.
- Errors still go to standard output, as in Bonus Project 1.
- The hash join's partitions are uneven for small INT keys. The grouping's new hash finaliser would fix
  that too, but it would move most of REPORT_M2's and REPORT_M4's numbers.
