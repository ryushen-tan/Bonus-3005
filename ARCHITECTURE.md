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

**The memory contract across M2 and M4.** M2 guarantees any plan of B ≥ 4 frames runs within them, so M4
never has to check that a plan fits. M4's cost model uses the same formulas as M2's operators, with the
frames M2 gives each one: B, or B − 1 below an operator that holds memory, which keeps a frame free to
write that input to a temporary file (DESIGN_M4.md section 4). A group_by with no keys holds none, and a
hash grouping is sized as M2 packs it, each group at its declared length with a slot. Where M2 spends
I/O the formulas do not count, that is measured in REPORT_M2.md rather than hidden.

## C++ implementation decisions

**C++17, standard library and POSIX only, header files with one `.cpp` per tool.** Every tool is a
single translation unit (`src/storage.cpp` is three lines: include `storage.hpp`, call `storageTool`), so
the `Makefile` needs no link step and no build system, and a test file includes exactly the headers of
the module it tests. The cost is that changing any header rebuilds every tool. The build uses `-Wall -Wextra -Werror`, so a warning is a failed build.

**Every tool is a function of its arguments and an `std::ostream`.** `main` only forwards `argv` and
`std::cout` to `storageTool`, `execTool` and the rest. The tests call the same functions with an
`std::ostringstream` and compare the whole output byte for byte, so they need no shell and no
subprocess, and what the tests check is exactly what the tool prints.

**Errors are one exception type, `Error { msg, line, col }`** (`lexer.hpp`). Every module throws it,
and each tool catches it once, at the top, and prints one line through `report`. A second
`catch (const std::exception&)` turns anything else (`std::bad_alloc`, `std::invalid_argument` from
`std::stoll`) into one `internal error:` line instead of an abort. Error codes would have had to be
checked at every one of the hundreds of places that can fail, and the position travels with the
exception from the scanner to the message.

**Resources are owned by objects, so an exception cannot leak a pin, a frame or a file (RAII).**
- `Frames` (`exec.hpp`) holds the frames an operator reserved from the pool and releases them in its
  destructor. It is non-copyable (`Frames(const Frames&) = delete`), so two owners cannot release the
  same frame.
- `HeapScan`, `RunReader` and `RunWriter` unpin or release their page in their destructors, and
  `ChildSource` closes its child.
- `TempFile` and `SpillFile` delete their temporary file in their destructors, which is why "no
  temporary files remain" holds even when a plan fails part way.
- `Charge` saves `DiskManager::attributed`, points it at the current operator's counters, and
  restores it in its destructor. Every `open`, `next` and `close` makes one, so each page read or
  written is charged to the operator that caused it, and nested operators restore their parent's
  counters on the way out.
- Where a function must unpin on failure and still rethrow, it does so explicitly
  (`catch (...) { pool.unpin(...); throw; }` in `heap.hpp`).

The "no pinned frames after any plan" checklist item comes from this rule, not from cleanup code at
the end of `execute`.

**Operators are a class hierarchy with the iterator interface as virtual functions.** `Operator`
(`exec.hpp`) has non-virtual `open`, `next` and `close`, which make the `Charge` and count
`tuplesOut`, and the subclasses override the virtual `start`, `produce` and `finish`. This is the
non-virtual interface pattern: the counting cannot be forgotten by any of the fifteen operators,
because none of them implements `next` itself. Children are
`std::vector<std::unique_ptr<Operator>>`, built by `buildNode` (`plan.hpp`) with `std::make_unique`,
so the plan tree owns its operators and destroying the root closes and frees everything. Virtual
dispatch costs one indirect call per tuple per operator. Next to page I/O it is not measurable, and a
template-based pipeline would have made the plan's shape a compile-time type when it is only known
from JSON at run time.

**Pages are bytes, encoded and decoded explicitly in little-endian.** `page.hpp` writes every integer
with `put16` and `put32` and reads it with `get16` and `get32`, a byte at a time with shifts, never by
casting a `uint8_t*` to a struct or an `int*`. This avoids undefined behaviour from misaligned access
and strict aliasing, and makes the file format the same on any machine. A `REAL` is copied with
`std::memcpy`, which is the defined way to move a `double`'s bytes. A frame is
`std::array<uint8_t, PAGE_SIZE>`, held by value inside the pool's `std::vector<Frame>`, so the pool's
memory is allocated once when it opens and never during a query.

**Disk I/O is POSIX `pread` and `pwrite` on file descriptors, not `fstream`.** A page read is one
system call at an explicit offset with no stream position and no library buffer, so the counters
count exactly the reads and writes that happen. That is what makes the M1 checklist item "the same
command twice gives identical counters" hold. New files are opened with `O_CREAT | O_EXCL`, so
`create-table` cannot overwrite a file. The catalog is written to a `mkstemp` file and moved over
`catalog.db` with `rename(2)`, which is atomic, so a crash leaves the old catalog or the new one,
never half of each.

**Trees are values.** A Bonus 1 `Node` holds its children as `std::vector<Node>`, and `LogicalNode` and
`Json` do the same, so a tree is copied, moved and destroyed by the compiler-generated members with no
pointers to manage. The optimizer's rewrite rules copy subtrees freely, which is cheap at plan sizes.
The physical operators are the exception, because they are polymorphic and need `unique_ptr`.

**Deep inputs run on a 1 GB stack.** The parsers, the optimizer's passes and the operators all recurse
once per level of a plan. The deepest inputs `GRAMMAR_SQL.md` allows overflow the 8 MB stack of a main
thread, so `onLargeStack` (`cli.hpp`) runs the tool on a `pthread` with `TOOL_STACK_BYTES` of stack.
Past those limits, `mk` (`tree.hpp`) refuses a tree nested deeper than `MAX_DEPTH` or taller than
`MAX_HEIGHT` with an `Error`, so an extreme input is an error message, not a stack overflow.

**Containers are chosen for determinism.** The buffer pool's page table and the optimizer's DP table
are `std::map`, ordered by key, so their iteration order, and with it the printed DP table, is the same
on every run and every standard library. The DP table is indexed by a `uint32_t` bitmask
of the relations in a subset (`best[1u << i]`), which caps a join at 32 relations, well above the SQL
front end's limit of 16 tables in one FROM.

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
