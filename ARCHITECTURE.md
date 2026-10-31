# ARCHITECTURE.md

One program, `db`, takes SQL and returns rows. It is built from four modules that each also run on their
own as a command line tool (`storage`, `exec`, `sql`, `opt`). Underneath is the Bonus Project 1
relational algebra library (`ra`). Everything is C++17 with no dependencies beyond the standard library,
in header files under `src/`, one `.cpp` per tool.

```
  SQL text                                   db> SELECT ... ;
     |
     v
  [ M3 sql ]   sqlparse.hpp: scanner -> parser -> syntax tree
               sqlplan.hpp:  scopes, names, types, aggregates -> logical plan in canonical order
     |
     |  logical plan: LogicalNode, or its JSON (logical.hpp)
     v
  [ M4 opt ]   optimizer.hpp: rules 1-5 -> join order by DP -> rules 6-7 -> estimates -> algorithms
     |
     |  physical plan JSON (the same format exec reads from a file)
     v
  [ M2 exec ]  plan.hpp: check -> build operators (names and types resolved) ; exec.hpp: open / next / close
     |
     |  fetch / unpin / reserve / release, and temporary pages through the disk manager
     v
  [ M1 storage ] buffer.hpp -> heap.hpp -> page.hpp -> disk.hpp, with catalog.hpp read by M3 and M4
                       ^
                       |
              catalog and statistics (catalog.db), written by ANALYZE, read by M3 and M4

  Bonus 1 library, used by all of them:
  lexer.hpp, tree.hpp, parser.hpp (conditions: parseCondition, condText), algebra.hpp (Col, Row, bind, eval),
  treejson.hpp (the algebra tree as plan JSON), json.hpp
```

`src/shell.hpp` is the integration. It splits input into statements and handles `EXPLAIN`, `EXPLAIN
ANALYZE`, `ANALYZE t [buckets]`, `SET frames = n` and `LOAD t FROM 'file'` itself. SQL goes through M3,
M4 and M2 by calling the same functions the tools call: `SqlCompiler::query`, `Optimizer::optimize`,
`physicalJson`, `buildPlan` and `execute`. Each statement is parsed where it stands, so an error gives
its line and column in the whole input, as `sql check` does, and text after the last semicolon is run
(or reported) at the end of the input. Integration was wiring. No module was rewritten for it, and the
four tools still work. `tests/e2e/run.py` chains the four tools by file, which is the strongest check
that each contract is enough on its own, and runs each query through the shell as well.

## The interface between each pair

**Bonus 1 library and everything.**
- Conditions are text in the Bonus 1 language (`R.a > 500 and R.b = 'cs'`) wherever they cross a module
  boundary. M3 writes them with `condText`, M4 reads them with `parseCondition` to split, rename and
  estimate them and writes them back with `condText`, and M2 parses them with `parseCondition`, resolves
  them with `bindCond` and evaluates them with `eval`. One grammar, one printer and one evaluator serve
  all four modules.
- `Col` (relation, name, type, length), `Row` and `Value` are the shared vocabulary: a schema is a
  `std::vector<Col>`, a tuple is a `Row`.
- `resolve` is the single definition of what `Name` and `Rel.Name` mean, used by M2's binding and M4's
  rules.

**M1 and M2.**
- M2 reads tables only through `HeapScan`, which pins one page at a time, and decodes records with M1's
  `decode`.
- Working memory is frames: `BufferPool::reserve` and `release`.
- Temporary files are `DiskManager::createTemp`, with `readPage` and `writePage`. They are not cached in
  the pool, so their reads and writes are exactly the textbook's.
- The disk manager's `attributed` pointer is how M2 charges each page access to the operator that made
  it. That pointer is the one thing M1 knows about M2.

**M1 and M3, M4.** The catalog, `Database::tables`: table names, columns with types, data pages, live
tuples, and the statistics ANALYZE writes. M3 needs only the schemas. M4 needs the statistics and page
counts. Both read it after the database is opened, which reads `catalog.db` through the pool and then
empties the pool, so each command's counters start from nothing.

**M3 and M4.** The logical plan: `scan`, `select`, `project`, `join`, `cross`, `rename`, `group_by`,
`distinct`, `sort`, `limit`, `union`, `union_all`, `intersect`, `except`, with qualified column names
throughout. The only unqualified names are aggregate outputs, named by the rule of DESIGN_M3.md. In
process it is a `LogicalNode`; between tools it is the JSON, which `logicalFromJson` checks node by
node.

**M4 and M2.** The physical plan JSON: Module 2's fifteen operators, plus `rename` and an optional
`condition` on `nested_loop_join`. M4 adds `est_rows` and `est_cost` to every node, which M2's checker
allows and ignores, and which `EXPLAIN ANALYZE` puts beside the actual counts. The test of this contract
is that every plan M4 emits runs in M2 unedited. The optimizer tests and the end to end run do that
for 68 queries at several values of B.

**The memory contract across M2 and M4.** M2 guarantees any plan of B ≥ 4 frames runs within them, so
M4 never has to check that a plan fits. M4's cost model uses the same formulas as M2's operators, with
the frames M2 gives each one: B, or B − 1 below an operator that holds memory, which keeps a frame free
to write that input to a temporary file (DESIGN_M4.md section 4). Where M2 spends I/O the formulas do
not count, that is measured in REPORT_M2.md rather than hidden.

## What I would do differently

**Decide the memory model in week one, not in Module 2.** The rule that only one operator holds memory
at a time, with inputs that hold memory materialised first, came from finding that a hash join over a
hash join could not run at B = 4. It works, and it made every plan runnable at B = 4. But it costs a
write and a read of every intermediate result between two memory-holding operators. It also turns the
textbook argument for left-deep trees, pipelining the outer, into something this system only half does.
It also took two bugs to get right: frames for nested materialisation stacking up, and then being
counted twice. If I started again I would design memory grants with the optimizer from the beginning.
The optimizer knows the plan's shape and the estimated sizes, so it can give each operator a budget
that the plans' pipelines can share, and charge the cost model for materialisation only where it really
happens. Module 2 would then accept a grant per node in the plan JSON.

**Give the catalog an update in place.** Rewriting `catalog.db` whole on every change is simple and
deterministic, but `INSERT` in the shell rewrites it per row. The catalog also had a real bug from this
design: first fit put a column record on an earlier page than its table record once statistics made the
file span pages, and reading assumed file order. A catalog with a fixed page for each kind of record, or
records read into memory and applied in dependency order (which is what the fix does), should have been
the design rather than the fix.

**Use the cost model's own numbers to pick the hash join's partition count.** Module 2 uses P = B − 1 as
specified. That is ruinous for small inputs at large B (8.8 times the formula at B = 256), because each
of 255 partitions writes a part-full page. The optimizer already knows the estimated input size, and
P = ceil(M / (B − 2)), bounded by B − 1, would avoid it.

**Keep statistics on column pairs.** The worst estimates in REPORT_M4.md all come from one column's
statistics being used for something that depends on two: correlated filter columns, two-column joins,
the distinct values of a combination. A functional dependency measure or a count of distinct pairs,
collected by the same single scan of ANALYZE, would have fixed three of the five worst cases.

**Write the end to end check against PostgreSQL first.** It found no wrong answer at the end, but it
would have been the fastest way to find the four modules' disagreements while they were being written,
rather than from each module's own tests. And it should have run the shell from the start: it only
chained the tools by file, so it never saw that every query with a LIMIT crashed the shell.
