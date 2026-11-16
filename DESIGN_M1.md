# DESIGN_M1.md: storage engine and buffer pool

The code is in `src/disk.hpp` (the only code that opens a file), `src/page.hpp` (byte layouts),
`src/buffer.hpp`, `src/heap.hpp`, `src/catalog.hpp`, and `src/storage.hpp` for the `storage` tool. The
tests are in `tests/test_storage.cpp`.

## 1 Files and addressing

A database is a directory. It holds `catalog.db` and one `<table>.db` file per table. Every file is a
sequence of 4096 byte pages and nothing else. A page is addressed by `PageId{file, page}`. The file is the
disk manager's index for the open file. The page is the page number inside that file, counting from zero.
That is the page id the module document talks about. Every multi-byte integer is little-endian.

A table's file is only made when the table is created (`storage create-table` or SQL's CREATE TABLE), and
never over a file that already exists. Everything else opens a file only if it is already there. So a
missing `R.db` is an error, not a new empty table. The error shows up when the file is first read or
written, not when the catalog is read. This means a database with one missing table file still has its
other tables. A scan, a load or a new table elsewhere still works. Any command on the broken table says
`io error: cannot open D/R.db: No such file or directory`. Reading a page past the end of a file is also
an error, so a file that was cut short can't be read as zeros. In the same way, only `create-table` (and
the shell) can make a new database. `load`, `scan` and `layout` on a directory with no `catalog.db` say it
is not a database. The `storage` tool prints every error on one line and exits with status 1. If an
exception isn't one of the engine's own errors, it prints `internal error: ...` instead of aborting.

The disk manager counts one read for each `readPage` and one write for each `writePage`. It keeps a total.
When the executor has set one, it also keeps the counters of the operator that is running (Module 2 uses
this to charge each operator for its own temporary files). `allocatePage` only moves the file's page count
forward. The page reaches the disk when the buffer pool writes it.

## 2 Record format

A record is the fields in schema order and nothing else. INT is 4 bytes, two's complement. VARCHAR(n) is a
2 byte length L followed by L bytes. The executor also writes a REAL (only the output of AVG) as the 8
bytes of a double, but only into its own temporary files. A table can never have one. When a record is
decoded, each field is checked against the record's length. A record shorter than its schema says can only
come from a damaged file. It gives the error `corrupt record` instead of reading past the end.

Here is a real record, `(42, 'bob', 7)`, from table `R(a INT, b VARCHAR(20), c INT)`, read from the file
with `xxd`:

```
2a 00 00 00     a = 42
03 00 62 6f 62  b = length 3, "bob"
07 00 00 00     c = 7                        13 bytes
```

## 3 Data page

The data page follows the layout in the module document exactly. This is a dump of page 1 of `R.db` after
loading `(42,bob,7)`, `(43,abcdefghij,9)`, `(44,amy,1)`. `storage layout --page 1` prints the same numbers.

```
offset 0x1000 (page 1 starts at byte 4096 of R.db)
  0  01 00 00 00   page_id     = 1
  4  03 00         slot_count  = 3
  6  d2 0f         free_end    = 0x0fd2 = 4050
  8  f3 0f 0d 00   slot 0      = (4083, 13)
 12  df 0f 14 00   slot 1      = (4063, 20)
 16  d2 0f 0d 00   slot 2      = (4050, 13)
 20  00 ...        free space, 4030 bytes, offsets 20 to 4049
4050  2c 00 00 00 03 00 61 6d 79 01 00 00 00                       (44, 'amy', 1)   newest, lowest
4063  2b 00 00 00 0a 00 61 62 63 64 65 66 67 68 69 6a 09 00 00 00  (43, 'abcdefghij', 9)
4083  2a 00 00 00 03 00 62 6f 62 07 00 00 00                       (42, 'bob', 7)   oldest, at the end
4096  end of page
```

These are the three inserts of the worked example in section 3.2 (13, 20 and 13 bytes). The test
`3.2 layout command after the delete` deletes slot 1. Then it checks that the layout command prints slot 1
as `(0, 0)`, that `free_end` is still 4050 and that the free space is still 4030.

A slot whose offset is 0 is dead. Offset 0 can never be a live record because the header is there. Slots
are never reused or renumbered, so a rid stays valid for as long as its record lives. Reading a dead or
missing slot is an error that names the rid. A rid's slot is compared as the unsigned number it is, so a
slot of 2^31 or more counts as missing, not negative. `get` and `remove` first look up the rid's page in
the directory. So if a rid points to a page the directory doesn't list (a directory page above all), the
error is `no record at rid (0, 0), page 0 is not a data page of table R`. Without this check, a directory
page could be read or overwritten as if it were data. For a table of up to 681 data pages, this takes one
directory page and the data page, which are the two page accesses the module allows.

### Compaction: not implemented

Deleting frees no space on the page (see the last row of the worked example). I decided not to compact,
for three reasons. First, nothing in the SQL subset deletes. Second, compaction would have to keep slot
numbers, so it could only move record bytes and rewrite offsets. That is cheap, but it would change the
free space in a directory entry on every delete, so the delete path would have to touch the directory too.
Third, the counters the module is graded on would then depend on when compaction ran. The cost is that a
page that had records deleted keeps that space until the table is reloaded.

## 4 Heap file and free space

Page 0 of every table file is the first directory page. It is laid out as in the module document:

```
offset 0x0000 (page 0 of R.db)
  0  00 00 00 00   page_id        = 0
  4  ff ff ff ff   next_dir_page  = none
  8  01 00         entry_count    = 1
 10  01 00 00 00   entry 0: data_page_id = 1
 14  be 0f                  free_bytes   = 4030
 16  00 ...        room for 680 more entries (681 per page)
```

For free space I use first fit through the directory chain, as the module document says. An insert of L
bytes looks for the first entry with `free_bytes >= L + 4`. It fetches that data page, inserts the record,
writes the page's new free space back into the entry, and unpins both pages as dirty. If no entry has
room, it allocates a new data page and adds it to the last directory page. If that directory page already
holds 681 entries, it first allocates a new directory page and links it from `next_dir_page`. Directory
pages and data pages share the file's page numbers, so in a large table they are mixed together (the
second directory page of a 700 page table is page 682). The test `a second directory page after 681 data
pages` loads 700 data pages. It checks the chain, the count, and that a scan returns every record in
insert order.

A new directory page is allocated when the chain grows, after every page already in the file. So
`next_dir_page` always points forward. Every walk of the chain (insert, scan, `get`, `remove` and `layout`)
checks this. If a chain points back, as page 0 of a file of zeros points at itself, the error is `corrupt
table file R.db: directory page 0 links back to page 0`. Without the check it would loop forever.

A record of more than 4084 bytes (4088 less its slot) is refused by the insert before any page is touched.
The message is `table R: a record of 4085 bytes does not fit on a page (at most 4084)`. The loader checks
every row before it inserts any, so its message names the row as well. An insert holds its directory page
and its data page pinned together, as the module describes. So `load` and `create-table` need B ≥ 2, and
they stop with `no unpinned frame available` at B = 1.

With first fit, a small record can go back into an early page that a large record couldn't fit into. That
is the reason for the directory. It is also why the measured tuples per page in REPORT_M1.md can differ
from a sequential packing when record lengths vary.

`num_pages` in the catalog counts data pages only. This is the `n_pages(T)` of Module 4. The file also
holds one directory page for every 681 data pages.

### The scan pins one page at a time

The module allows one data page and one directory page to be pinned during a scan. My scan only pins one.
When it reaches a directory page, it copies that page's list of data page ids (at most 681 ids, 2.7 KB)
and unpins the page before it fetches the first data page. After that it keeps one data page pinned while
it hands out that page's live records, and unpins it before it fetches the next one. I did this because of
Module 2. A `seqscan` there may hold one frame. The hash join's partition phase uses B − 1 frames for its
partitions plus one for the frame its input arrives in. If a directory page were pinned too, every scan
would need two frames, and the hash join couldn't run at B = 4. The page list is metadata and is smaller
than a page. It is the one thing a scan keeps outside the pool. The test `a scan of a table twenty times B
pins one page at a time` scans 160 data pages at B = 8. It checks that the pool's peak of pinned frames
was 1 and that the reads were exactly 161 (160 data pages and 1 directory page).

## 5 Buffer pool

The pool is a vector of B frames. Each frame holds the page's bytes, a pin count, a dirty flag, a
reference bit and the time of its last access. There is also a `std::map` from page id to frame. `fetch`,
`unpin`, `newPage`, `flushAll` and `stats` work as in the module document, and the counters follow its
rules exactly. A hit is a fetch of a page that is already in the pool. A miss is a fetch that had to read
from disk. An eviction is a miss that replaced a page already in the pool, so the first B misses on an
empty pool are not evictions. Reads and writes are the disk manager's calls.

To pick a victim, an empty frame is always used first, lowest index first. After that it depends on the
policy.

LRU (least recently used) stamps the frame with an increasing access number on every fetch, hit or miss.
The victim is the unpinned frame with the smallest stamp. This is a linear search over B frames. For B ≤
256 that costs less than keeping a list in order, and it can't get out of step with the pins.

CLOCK follows the module document's pseudocode line for line. The sweep passes over pinned frames and
counts them. It clears a set reference bit and counts that too. The first unpinned frame with a clear bit
is the victim. If the sweep takes more than 2B steps, every frame is pinned. Every fetch sets the frame's
reference bit. After a page is loaded into frame f, the hand is at f + 1. That matches "the hand advances
past it" both when f was chosen by the sweep and when f was empty.

A pinned page can never be evicted. Neither policy will choose a frame with a pin. When there is no
unpinned frame, `fetch` fails with `no unpinned frame available`. Unpinning a page that isn't in the pool,
or whose pin count is already 0, is an error. Both cases have tests, under both policies. A victim is
emptied before anything is read into its frame. Its page is written back if it is dirty and dropped from
the page table. So if the read then fails (a page past the end of a file, or a missing file), the frame is
left empty and clean. It doesn't claim a page the table no longer maps, and no miss or eviction is
counted.

A pool needs at least one frame. Every tool's `--frames` value B becomes the size of a pool, so the pool's
constructor is the one place that checks B ≥ 1, with one message, `--frames must be at least 1`. `opt` and
the shell check a stricter bound of their own, B ≥ 4, which is the least a plan needs (section 4 of
DESIGN_M4.md). CLOCK is the default policy, for the reasons in section 2 of REPORT_M1.md. `--policy lru`
selects LRU.

I added two things that the later modules need.

`reserve()` takes a frame out of the pool to use as working memory for an operator (the run buffer of a
sort, or the partitions of a hash join). It chooses the frame the way a fetch would and writes it back if
it was dirty. The frame counts as pinned until `release()`. This is how Module 2 keeps its memory budget
inside the pool and not beside it. An operator that wants k pages of memory has to get k frames, so the
plan as a whole can never hold more than B. `peak` records the most frames that were pinned or reserved at
once.

`clear()` writes back every dirty page, empties every frame and zeroes the counters. Every command reads
the catalog through the pool and then clears it. So the counters a command reports describe only that
command, and they start from the same empty pool every time. That is what makes two runs of the same scan
give identical counters (this is tested).

## 6 Catalog

`catalog.db` is itself a heap file. It has the same directory and data pages as any table and is read
through the buffer pool. Each record is one fact, encoded with the record format above. The first field of
every record is its kind.

| kind | fields |
|---|---|
| 1 table | kind INT, table VARCHAR(255), file VARCHAR(255), data_pages INT, live_tuples INT |
| 2 column | kind INT, table, position INT, name VARCHAR(255), type INT (0 INT, 1 VARCHAR), length INT |
| 3 table statistics | kind INT, table, tuples INT, pages INT, width VARCHAR(255) (a double written with 17 digits) |
| 4 column statistics | kind INT, table, position INT, ndv INT, min VARCHAR(255), max VARCHAR(255), width VARCHAR(255) (average bytes) |
| 5 histogram bucket | kind INT, table, position INT, index INT, lo, hi VARCHAR(255), count INT, ndv INT |

Kinds 3 to 5 are the room the module document asks me to leave for Module 4. Values of either type are
stored as text, because a column's minimum may be an INT or a VARCHAR. They are turned back into values,
using the column's type, when the catalog is read. Statistics are split into one record per bucket. That
keeps every record far below the 4084 byte limit, however many buckets there are.

A table's name is also its file's name. So `create-table`, whether from the storage tool or SQL, only
accepts an identifier (a letter or underscore, then letters, digits and underscores) of at most 252
characters. That leaves room for `.db` in the 255 bytes a file name may have. It refuses `catalog` in any
case, since that is the name of the catalog's own file. It also refuses a name that matches an existing
table's name when case is ignored. On a file system that ignores case, like macOS's, those two tables
would share one file. A column name may have at most 255 characters, the VARCHAR(255) the catalog stores
it in, so every catalog record fits on a page.

Here are the real first records of a catalog with one table, from the dump of `catalog.db`, end of page 1:

```
01 00 00 00  01 00 52  04 00 52 2e 64 62  01 00 00 00  03 00 00 00
kind 1       "R"       "R.db"             1 data page   3 tuples
```

When the catalog changes (create-table, load, ANALYZE, an INSERT in the shell), I write all of it to a new
temporary file in the database directory, flush it, and rename it over `catalog.db`. A rename replaces the
file in one step. So if the process is killed at any moment, the old catalog or the new one is left, never
half of each. The temporary file is made with mode 0600. Before the rename it gets the mode every other
file is created with, which is 0644 less the umask. Under umask 077, `catalog.db` is 0600 like the table
files. If a save fails, its temporary file is deleted. If the save of a `create-table` fails, the table
file it made is deleted too, so the name can be used again. The catalog is small and the rewrite is
deterministic. That means running ANALYZE twice produces a byte-identical `catalog.db`.

## 7 Loading

`storage load` reads the CSV file twice. The first pass checks every line. It checks the number of fields.
It checks that every INT is a number that fits in 32 bits (it reads the value, so `000000000001` and `+7`
are INTs). It checks that every VARCHAR is well-formed UTF-8 and fits its length in bytes. It also checks
that the encoded record fits on a page. The second pass does the inserts. So a file with a bad line 9000
loads nothing, instead of loading 8999 rows. The error names the line and the column (`line 2, column 1
(a): 'x' is not an INT`), or the table and row for a record over 4084 bytes. Bytes that are not UTF-8 are
named by position, as in `line 2, column 2 (b): byte 2 is not valid UTF-8`, so the error doesn't repeat
them. This also means whatever `load` stores, `scan --json` prints as valid JSON. A field that has a comma
in it is wrapped in double quotes, and inside quotes `""` is one quote, as in RFC 4180. A `\r` before the
newline is dropped, so files written on Windows load.

## 8 Sources and what they changed

Ramakrishnan and Gehrke, *Database Management Systems*, chapter 9 (storage and buffer management). Their
treatment of slotted pages for variable length records is why a rid is (page, slot) with a slot directory
that never renumbers. Their discussion of sequential flooding under LRU is why trace 3 exists, and it is
the explanation used in REPORT_M1.md.

O'Neil, O'Neil and Weikum, *The LRU-K Page Replacement Algorithm for Database Disk Buffering*, SIGMOD
1993. LRU ranks a page by its last reference alone. So one pass of a scan evicts pages that are
referenced again and again. That is the failure trace 4 shows. It is the argument in the report for
keeping LRU and CLOCK simple and handling scans separately, instead of making the replacement policy
smarter.

Hellerstein, Stonebraker and Hamilton, *Architecture of a Database System*, the section on buffer
management. It describes the buffer pool as the owner of all of the engine's page memory. That is why
operator working memory is reserved frames inside the pool here, not a separate allowance beside it.
