# ARCHITECTURE.md

There is one program, `db`. It takes SQL and returns rows. It is built from four modules, and each one also runs on its own as a command line tool (`storage`, `exec`, `sql`, `opt`). Underneath them is the Bonus Project 1 relational algebra library (`ra`). Everything is C++17 with only the standard library. The code is in header files under `src/`, with one `.cpp` per tool.

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

`src/shell.hpp` ties the modules together. It splits the input into statements. It handles `EXPLAIN`, `EXPLAIN ANALYZE`, `ANALYZE t [buckets]`, `SET frames = n` and `LOAD t FROM 'file'` itself. Any SQL goes through M3, M4 and M2 by calling the same functions the tools call: `SqlCompiler::query`, `Optimizer::optimize`, `physicalJson`, `buildPlan` and `execute`. Each statement is parsed where it stands, so an error gives its line and column in the whole input, as `sql check` does. Text after the last semicolon is run (or reported) at the end of the input. Putting it all together was just wiring. I didn't have to rewrite any module for it, and the four tools still work. `tests/e2e/run.py` chains the four tools by file, which is the strongest check that each contract is enough on its own. It also runs each query through the shell.

## The interface between each pair

Bonus 1 library and everything. Wherever a condition crosses from one module to another, it is text in the Bonus 1 language (`R.a > 500 and R.b = 'cs'`). M3 writes conditions with `condText`. M4 reads them with `parseCondition` so it can split, rename and estimate them, then writes them back with `condText`. M2 parses them with `parseCondition`, resolves them with `bindCond` and evaluates them with `eval`. So one grammar, one printer and one evaluator serve all four modules. `Col` (relation, name, type, length), `Row` and `Value` are the shared types. A schema is a `std::vector<Col>` and a tuple is a `Row`. `resolve` is the one place that defines what `Name` and `Rel.Name` mean. M2's binding and M4's rules both use it.

M1 and M2. M2 reads tables only through `HeapScan`, which pins one page at a time, and it decodes records with M1's `decode`. Working memory is frames, which M2 takes with `BufferPool::reserve` and gives back with `release`. Temporary files use `DiskManager::createTemp`, with `readPage` and `writePage`. They are not cached in the pool, so their reads and writes match the textbook exactly. The disk manager's `attributed` pointer is how M2 charges each page access to the operator that made it. That pointer is the one thing M1 knows about M2.

M1 and M3, M4. They share the catalog, `Database::tables`. It holds the table names, the columns with their types, the data pages, the live tuples, and the statistics that ANALYZE writes. M3 only needs the schemas. M4 needs the statistics and the page counts. Both read the catalog after the database is opened. Opening reads `catalog.db` through the pool and then empties the pool, so each command's counters start from zero.

M3 and M4. They share the logical plan. Its node types are `scan`, `select`, `project`, `join`, `cross`, `rename`, `group_by`, `distinct`, `sort`, `limit`, `union`, `union_all`, `intersect` and `except`, with qualified column names everywhere. The only unqualified names are aggregate outputs, which are named by the rule in DESIGN_M3.md. Inside one process the plan is a `LogicalNode`. Between tools it is JSON, and `logicalFromJson` checks it node by node.

M4 and M2. They share the physical plan JSON. It uses Module 2's fifteen operators, plus `rename` and an optional `condition` on `nested_loop_join`. M4 adds `est_rows` and `est_cost` to every node. M2's checker allows these and ignores them, and `EXPLAIN ANALYZE` puts them next to the actual counts. The test of this contract is that every plan M4 produces runs in M2 without editing. The optimizer tests and the end to end run do this for 68 queries at several values of B.

The memory contract across M2 and M4. M2 guarantees that any plan runs within B frames as long as B is at least 4. So M4 never has to check that a plan fits. M4's cost model uses the same formulas as M2's operators, with the frames M2 gives each one. That is B, or B − 1 below an operator that holds memory, which keeps one frame free to write that input to a temporary file (DESIGN_M4.md section 4). A group_by with no keys holds no memory. A hash grouping is sized the way M2 packs it, with each group at its declared length plus a slot. Some of M2's I/O isn't counted by the formulas. I measured that in REPORT_M2.md instead of hiding it.

## C++ implementation decisions

I used C++17, the standard library and POSIX only, with header files and one `.cpp` per tool. Every tool is a single translation unit (`src/storage.cpp` is three lines: it includes `storage.hpp` and calls `storageTool`). So the `Makefile` needs no link step and no build system, and a test file includes just the headers of the module it tests. The downside is that changing any header rebuilds every tool. The build uses `-Wall -Wextra -Werror`, so a warning fails the build.

Every tool is a function of its arguments and a `std::ostream`. `main` only passes `argv` and `std::cout` to `storageTool`, `execTool` and the rest. The tests call the same functions with a `std::ostringstream` and compare the whole output byte for byte. They don't need a shell or a subprocess, and what they check is exactly what the tool prints.

There is one exception type for errors, `Error { msg, line, col }` (in `lexer.hpp`). Every module throws it. Each tool catches it once, at the top, and prints one line through `report`. A second `catch (const std::exception&)` turns anything else (like `std::bad_alloc`, or `std::invalid_argument` from `std::stoll`) into one `internal error:` line instead of an abort. If I had used error codes, I would have had to check them at each of the hundreds of places that can fail. With an exception, the position travels from the scanner to the message.

Objects own their resources, so an exception can't leak a pin, a frame or a file (this is called RAII). Here is how each one works.
- `Frames` (`exec.hpp`) holds the frames an operator reserved from the pool and releases them in its destructor. It can't be copied (`Frames(const Frames&) = delete`), so two owners can't release the same frame.
- `HeapScan`, `RunReader` and `RunWriter` unpin or release their page in their destructors, and `ChildSource` closes its child.
- `TempFile` and `SpillFile` delete their temporary file in their destructors. That is why "no temporary files remain" holds even when a plan fails part way.
- `Charge` saves `DiskManager::attributed`, points it at the current operator's counters, and puts the old value back in its destructor. Every `open`, `next` and `close` makes one. So each page read or written is charged to the operator that caused it, and nested operators restore their parent's counters on the way out.
- Where a function has to unpin on failure and still rethrow, it does that by hand (`catch (...) { pool.unpin(...); throw; }` in `heap.hpp`).

The "no pinned frames after any plan" checklist item comes from this rule, not from cleanup code at the end of `execute`.

Operators are a class hierarchy, and the iterator interface is made of virtual functions. `Operator` (`exec.hpp`) has non-virtual `open`, `next` and `close`. These make the `Charge` and count `tuplesOut`. The subclasses override the virtual `start`, `produce` and `finish`. This is called the non-virtual interface pattern. It means none of the fifteen operators can forget the counting, because none of them implements `next` itself. Children are `std::vector<std::unique_ptr<Operator>>`, built by `buildNode` (`plan.hpp`) with `std::make_unique`. So the plan tree owns its operators, and destroying the root closes and frees everything. Virtual dispatch costs one indirect call per tuple per operator. Next to page I/O that is too small to measure. A template-based pipeline would have made the plan's shape a compile-time type, but the shape is only known from JSON at run time.

Pages are bytes, and I encode and decode them by hand in little-endian. `page.hpp` writes every integer with `put16` and `put32` and reads it with `get16` and `get32`. These work one byte at a time with shifts, and never cast a `uint8_t*` to a struct or an `int*`. That avoids undefined behaviour from misaligned access and strict aliasing, and it makes the file format the same on any machine. A `REAL` is copied with `std::memcpy`, which is the defined way to move a `double`'s bytes. A frame is a `std::array<uint8_t, PAGE_SIZE>`, held by value inside the pool's `std::vector<Frame>`. So the pool's memory is allocated once when it opens and never during a query.

Disk I/O uses POSIX `pread` and `pwrite` on file descriptors, not `fstream`. A page read is one system call at an explicit offset, with no stream position and no library buffer. So the counters count exactly the reads and writes that happen. That is what makes the M1 checklist item "the same command twice gives identical counters" hold. New files are opened with `O_CREAT | O_EXCL`, so `create-table` can't overwrite a file. The catalog is written to a `mkstemp` file and then moved over `catalog.db` with `rename(2)`. That move is atomic, so a crash leaves the old catalog or the new one, never half of each.

Trees are values. A Bonus 1 `Node` holds its children as `std::vector<Node>`, and `LogicalNode` and `Json` do the same. So the compiler-generated members copy, move and destroy a tree, and there are no pointers to manage. The optimizer's rewrite rules copy subtrees freely, which is cheap at plan sizes. The physical operators are the exception, because they are polymorphic and need `unique_ptr`.

Deep inputs run on a 1 GB stack. The parsers, the optimizer's passes and the operators all recurse once per level of a plan. The deepest inputs `GRAMMAR_SQL.md` allows would overflow the 8 MB stack of a main thread. So `onLargeStack` (`cli.hpp`) runs the tool on a `pthread` with `TOOL_STACK_BYTES` of stack. Past those limits, `mk` (`tree.hpp`) refuses a tree nested deeper than `MAX_DEPTH` or taller than `MAX_HEIGHT`, with an `Error`. So an extreme input gives an error message, not a stack overflow.

I picked containers for determinism. The buffer pool's page table and the optimizer's DP table are `std::map`, which is ordered by key. So their iteration order, and with it the printed DP table, is the same on every run and every standard library. The DP table is indexed by a `uint32_t` bitmask of the relations in a subset (`best[1u << i]`). That limits a join to 32 relations, which is well above the SQL front end's limit of 16 tables in one FROM.

## What I would do differently

Decide the memory model in week one, not in Module 2. The rule is that only one operator holds memory at a time, and inputs that hold memory are materialised first. I came up with it after finding that a hash join over a hash join could not run at B = 4. It works, and it made every plan runnable at B = 4. But it costs a write and a read of every intermediate result between two memory-holding operators. It also turns the textbook argument for left-deep trees (pipelining the outer input) into something this system only half does. It took two bugs to get right too. First, frames for nested materialisation stacked up, and then they were counted twice. If I started again, I would design memory grants with the optimizer from the beginning. The optimizer knows the plan's shape and the estimated sizes. So it can give each operator a budget that the plans' pipelines can share, and charge the cost model for materialisation only where it really happens. Module 2 would then accept a grant per node in the plan JSON.

Give the catalog an update in place. Rewriting `catalog.db` whole on every change is simple and deterministic, but `INSERT` in the shell rewrites it for every row. This design also caused a real bug in the catalog. Once statistics made the file span pages, first fit put a column record on an earlier page than its table record, and reading assumed file order. The design should have been a catalog with a fixed page for each kind of record, or records read into memory and applied in dependency order. That is what the fix does, but it should have been the design from the start.

Use the cost model's own numbers to pick the hash join's partition count. Module 2 uses P = B − 1, as specified. That is very bad for small inputs at large B (8.8 times the formula at B = 256), because each of the 255 partitions writes a page that is only partly full. The optimizer already knows the estimated input size, and P = ceil(M / (B − 2)), capped at B − 1, would avoid this.

Keep statistics on column pairs. The worst estimates in REPORT_M4.md all come from using one column's statistics for something that depends on two. Examples are correlated filter columns, two-column joins, and the distinct values of a combination. A functional dependency measure or a count of distinct pairs, collected by the same single scan of ANALYZE, would have fixed three of the five worst cases.

Write the end to end check against PostgreSQL first. It found no wrong answer in the end. But it would have been the fastest way to find where the four modules disagreed while I was writing them, instead of finding out from each module's own tests. It should also have run the shell from the start. It only chained the tools by file, so it never saw that every query with a LIMIT crashed the shell.
