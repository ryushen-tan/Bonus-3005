# DESIGN_M1.md: storage engine and buffer pool

Code: `src/disk.hpp` (the only code that opens a file), `src/page.hpp` (byte layouts), `src/buffer.hpp`,
`src/heap.hpp`, `src/catalog.hpp`, and `src/storage.hpp` for the `storage` tool. Tests: `tests/test_storage.cpp`.

## 1 Files and addressing

A database is a directory: `catalog.db` plus one `<table>.db` per table. Every file is a sequence of
4096 byte pages with nothing outside them. A page is addressed by `PageId{file, page}`: the file is the
disk manager's index of the open file, the page is the zero-based page number inside it, which is the
page id the module document talks about. Every multi-byte integer is little-endian.

The disk manager counts one read per `readPage` and one write per `writePage`, in a total and, when the
executor has set it, in the counters of the operator that is running (Module 2 uses that to charge each
operator for its own temporary files). `allocatePage` only advances the file's page count; the page
reaches the disk when the buffer pool writes it.

## 2 Record format

The fields in schema order, nothing else. INT is 4 bytes two's complement, VARCHAR(n) is a 2 byte length
L then L bytes. The executor also writes a REAL (only the output of AVG) as the 8 bytes of a double into
its own temporary files; a table can never have one.

The real record `(42, 'bob', 7)` of table `R(a INT, b VARCHAR(20), c INT)`, read from the file with `xxd`:

```
2a 00 00 00     a = 42
03 00 62 6f 62  b = length 3, "bob"
07 00 00 00     c = 7                        13 bytes
```

## 3 Data page

Exactly the layout in the module document. A dump of page 1 of `R.db` after loading
`(42,bob,7)`, `(43,abcdefghij,9)`, `(44,amy,1)`; `storage layout --page 1` prints the same numbers:

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

These are the three inserts of the worked example in section 3.2 (13, 20 and 13 bytes); the test
`3.2 layout command after the delete` deletes slot 1 and checks the layout command prints slot 1 as
`(0, 0)`, `free_end` still 4050 and the free space still 4030.

A slot whose offset is 0 is dead. Offset 0 can never be a live record because the header is there.
Slots are never reused or renumbered, so a rid stays valid for as long as its record lives, and
reading a dead or missing slot is an error naming the rid.

### Compaction: not implemented

Deleting frees no space on the page (the last row of the worked example). I decided not to compact,
for three reasons. Nothing in the SQL subset deletes. Compaction would have to keep slot numbers, so it
could only move record bytes and rewrite offsets, which is cheap but would make the free space in a
directory entry change on delete and so make the delete path touch the directory too. And the
counters the module is graded on would then depend on when compaction ran. The cost is that a page that
had records deleted keeps the space until the table is reloaded.

## 4 Heap file and free space

Page 0 of every table file is the first directory page, laid out as in the module document:

```
offset 0x0000 (page 0 of R.db)
  0  00 00 00 00   page_id        = 0
  4  ff ff ff ff   next_dir_page  = none
  8  01 00         entry_count    = 1
 10  01 00 00 00   entry 0: data_page_id = 1
 14  be 0f                  free_bytes   = 4030
 16  00 ...        room for 680 more entries (681 per page)
```

Free space strategy: first fit through the directory chain, as the module document specifies. An insert
of L bytes looks for the first entry with `free_bytes >= L + 4`, fetches that data page, inserts,
writes the page's new free space back into the entry and unpins both pages dirty. If no entry has room,
it allocates a data page and enters it in the last directory page; if that page already holds 681
entries it first allocates a new directory page and links it from `next_dir_page`. Directory pages and
data pages share the file's page numbers, so in a large table they interleave (the second directory page
of a 700 page table is page 682). The test `a second directory page after 681 data pages` loads 700
data pages and checks the chain, the count and that a scan returns every record in insert order.

First fit means a small record can go back into an early page that a large record could not fit into.
That is the reason for the directory, and it is why the measured tuples per page in REPORT_M1.md can
differ from a sequential packing when record lengths vary.

`num_pages` in the catalog counts data pages only, which is the `n_pages(T)` of Module 4. The file also
holds one directory page per 681 data pages.

### The scan pins one page at a time

The module allows one data page and one directory page pinned during a scan. My scan pins only one.
When it reaches a directory page it copies that page's list of data page ids (at most 681 ids, 2.7 KB)
and unpins it before it fetches the first data page. Then it holds one data page pinned while it hands
out that page's live records, and unpins it before it fetches the next. The reason is Module 2: a
`seqscan` there may hold one frame, and the hash join's partition phase uses B − 1 frames for its
partitions plus the one its input arrives in. With a directory page pinned as well, every scan would
need two frames and the hash join could not run at B = 4. The page list is metadata, smaller than a
page, and is the one thing a scan keeps outside the pool. The test
`a scan of a table twenty times B pins one page at a time` scans 160 data pages at B = 8 and checks
that the pool's peak of pinned frames was 1 and the reads were exactly 161 (160 data pages and 1
directory page).

## 5 Buffer pool

A vector of B frames, each with the page's bytes, a pin count, a dirty flag, a reference bit and the
time of its last access, plus a `std::map` from page id to frame. `fetch`, `unpin`, `newPage`,
`flushAll` and `stats` behave as in the module document, and the counters follow its rules exactly. A
hit is a fetch of a resident page. A miss is a fetch that read from disk. An eviction is a miss that
replaced a resident page, so the first B misses on an empty pool are not evictions. Reads and writes are
the disk manager's calls.

Victim choice. An empty frame is always used first, lowest index first. After that:

- LRU: every fetch, hit or miss, stamps the frame with an increasing access number. The victim is the
  unpinned frame with the smallest stamp. It is a linear search over B frames, which for B ≤ 256 costs
  less than keeping a list in order and cannot get out of step with the pins.
- CLOCK: the sweep is the module document's pseudocode line for line. Pinned frames are passed over and
  counted, a set reference bit is cleared and counted, the first unpinned frame with a clear bit is the
  victim, and more than 2B steps means every frame is pinned. Every fetch sets the frame's reference bit.
  After a page is loaded into frame f the hand is at f + 1, which is "the hand advances past it" both
  when f was chosen by the sweep and when f was empty.

Evicting a pinned page cannot happen: neither policy will choose a frame with a pin, and when there is
no unpinned frame `fetch` fails with `no unpinned frame available`. Unpinning a page that is not
resident, or whose pin count is already 0, is an error. Both have tests, under both policies.

Two additions that the later modules need:

- `reserve()` takes a frame out of the pool as working memory for an operator (the run buffer of a
  sort, the partitions of a hash join). It chooses the frame like a fetch would, writes it back if it was
  dirty, and counts as pinned until `release()`. This is how Module 2 keeps its memory budget inside the
  pool rather than beside it: an operator that wants k pages of memory has to get k frames, so the plan
  as a whole can never hold more than B. `peak` records the most frames pinned or reserved at once.
- `clear()` writes back every dirty page, empties every frame and zeroes the counters. Every command
  reads the catalog through the pool and then clears it, so the counters a command reports describe the
  command and start from the same empty pool every time. That is what makes two runs of the same scan
  give identical counters (tested).

## 6 Catalog

`catalog.db` is itself a heap file, with the same directory and data pages as any table, read through
the buffer pool. Each record is one fact, encoded with the record format above, and the first field of
every record is its kind:

| kind | fields |
|---|---|
| 1 table | kind INT, table VARCHAR(255), file VARCHAR(255), data_pages INT, live_tuples INT |
| 2 column | kind INT, table, position INT, name VARCHAR(255), type INT (0 INT, 1 VARCHAR), length INT |
| 3 table statistics | kind INT, table, tuples INT, pages INT, width VARCHAR(255) (a double written with 17 digits) |
| 4 column statistics | kind INT, table, position INT, ndv INT, min VARCHAR(255), max VARCHAR(255), width VARCHAR(255) (average bytes) |
| 5 histogram bucket | kind INT, table, position INT, index INT, lo, hi VARCHAR(255), count INT, ndv INT |

Kinds 3 to 5 are the room the module document asks to leave for Module 4. Values of either type are
stored as their text, because a column's minimum may be an INT or a VARCHAR, and are turned back into
values using the column's type when the catalog is read. Splitting statistics into one record per bucket
keeps every record far below the 4088 byte limit for any bucket count.

The real first records of a catalog with one table, from the dump of `catalog.db`, end of page 1:

```
01 00 00 00  01 00 52  04 00 52 2e 64 62  01 00 00 00  03 00 00 00
kind 1       "R"       "R.db"             1 data page   3 tuples
```

When the catalog changes (create-table, load, ANALYZE) it is rewritten whole: the pool forgets its
pages, the file is truncated, and the records are inserted again and flushed. The catalog is small, the
rewrite is deterministic, and it means running ANALYZE twice produces a byte-identical `catalog.db`.

## 7 Loading

`storage load` reads the CSV file twice. The first pass checks every line: the number of fields, every
INT parses and fits in 32 bits, every VARCHAR fits its length, and the encoded record fits on a page.
The second pass inserts. So a file with a bad line 9000 loads nothing rather than 8999 rows, and the
error names the line and the column (`line 2, column 1 (a): 'x' is not an INT`), or the table and row for
a record over 4088 bytes. A field containing a comma is wrapped in double quotes, and inside quotes `""`
is one quote, as in RFC 4180. A `\r` before the newline is dropped so files written on Windows load.

## 8 Sources and what they changed

- Ramakrishnan and Gehrke, *Database Management Systems*, chapter 9 (storage and buffer management).
  Its treatment of slotted pages for variable length records is why a rid is (page, slot) with a slot
  directory that never renumbers. Its discussion of sequential flooding under LRU is why trace 3 exists
  and is the explanation used in REPORT_M1.md.
- O'Neil, O'Neil and Weikum, *The LRU-K Page Replacement Algorithm for Database Disk Buffering*, SIGMOD
  1993. LRU ranks a page by its last reference alone, so one pass of a scan evicts pages that are
  referenced again and again. That is the failure trace 4 shows. It is the argument in the report for
  keeping LRU and CLOCK simple and handling scans separately rather than making the replacement policy
  smarter.
- Hellerstein, Stonebraker and Hamilton, *Architecture of a Database System*, section on buffer
  management. It describes the buffer pool as the owner of all of the engine's page memory. That is why
  operator working memory is reserved frames inside the pool here, not a separate allowance beside it.
