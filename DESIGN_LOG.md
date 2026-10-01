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
