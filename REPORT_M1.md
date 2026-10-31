# REPORT_M1.md: storage measurements

Machine: Apple M5 Pro, 24 GB, macOS 26.5.2, Apple clang 21.0.0, `-std=c++17 -O2`. Every number below
comes from `python3 perf/m1.py`, which writes the traces and the data, runs `./storage`, and prints these
tables into `perf/m1/results.md`. It takes under a second. It was run twice, and the second run printed
identical numbers.

The five traces the handout publishes were not on Brightspace when I did this, so `perf/m1.py` writes
five of its own with a fixed linear congruential generator. Trace 3 is the one the handout describes, a
sequential scan of an 80 page file repeated ten times. The traces are in `perf/m1/traces/`, and
`storage trace` runs any trace file, so the published ones can be dropped in.

## 1 Capacity

Records per page under my layout (section 3.3 of the module): a record of L bytes needs L bytes of data
and a 4 byte slot, and a page has 4096 − 8 = 4088 bytes after the header, so floor(4088 / (L + 4)).
5000 rows were loaded into each table, and `storage layout --page 1` gives the measured count on the
first data page.

| schema | record bytes L | computed per page | measured on page 1 | 5000 tuples / data pages |
|---|---|---|---|---|
| `a:INT,b:INT,c:INT` | 12 | floor(4088 / 16) = 255 | 255 | 5000 / 20 = 250.00 |
| `a:INT,b:VARCHAR(20),c:INT`, b = `bob` | 13 | floor(4088 / 17) = 240 | 240 | 5000 / 21 = 238.10 |
| `a:INT,b:VARCHAR(200)`, b 200 characters | 206 | floor(4088 / 210) = 19 | 19 | 5000 / 264 = 18.94 |

The computed and measured values per page agree exactly. A full page also wastes what is left after
the last record: 4088 − 255 × 16 = 8 bytes, 4088 − 240 × 17 = 8 bytes, and 4088 − 19 × 210 = 98 bytes.
The 98 bytes are almost half a record of the long schema; 19 × 210 = 3990 and a twentieth record would
need 4200.

The only gap is in the last column, and it is not a layout effect. 5000 is not a multiple of the
capacity, so the last page is part full: 19 full pages of 255 plus one page of 155, 20 pages of 240 plus
one of 200, 263 pages of 19 plus one of 3. The same figure is where the 1 in "tuples per page" estimates
of Module 4 comes from. A VARCHAR column whose values vary in length would also make the per page count
vary from page to page, because the capacity depends on the actual bytes, not the declared n: a
VARCHAR(200) column of 10 character strings packs like a 16 byte record, not a 206 byte one.

## 2 Replacement policies

Each trace was run under LRU and CLOCK with B = 4, 16, 64 and 256, with an empty pool and every page
unpinned clean straight after it is fetched. Hit rate is hits divided by accesses.

| trace | accesses | distinct pages | B | LRU hits | LRU hit rate | CLOCK hits | CLOCK hit rate |
|---|---|---|---|---|---|---|---|
| 1 uniform over 200 pages | 1000 | 199 | 4 | 0 | 0.000 | 0 | 0.000 |
| 1 uniform over 200 pages | 1000 | 199 | 16 | 81 | 0.081 | 80 | 0.080 |
| 1 uniform over 200 pages | 1000 | 199 | 64 | 302 | 0.302 | 296 | 0.296 |
| 1 uniform over 200 pages | 1000 | 199 | 256 | 801 | 0.801 | 801 | 0.801 |
| 2 80/20 skew over 200 pages | 1000 | 94 | 4 | 118 | 0.118 | 103 | 0.103 |
| 2 80/20 skew over 200 pages | 1000 | 94 | 16 | 455 | 0.455 | 440 | 0.440 |
| 2 80/20 skew over 200 pages | 1000 | 94 | 64 | 862 | 0.862 | 857 | 0.857 |
| 2 80/20 skew over 200 pages | 1000 | 94 | 256 | 906 | 0.906 | 906 | 0.906 |
| 3 sequential scan of 80 pages, ten times | 800 | 80 | 4 | 0 | 0.000 | 0 | 0.000 |
| 3 sequential scan of 80 pages, ten times | 800 | 80 | 16 | 0 | 0.000 | 0 | 0.000 |
| 3 sequential scan of 80 pages, ten times | 800 | 80 | 64 | 0 | 0.000 | 0 | 0.000 |
| 3 sequential scan of 80 pages, ten times | 800 | 80 | 256 | 720 | 0.900 | 720 | 0.900 |
| 4 hot set of 10 pages mixed with a 300 page scan | 600 | 310 | 4 | 0 | 0.000 | 0 | 0.000 |
| 4 hot set of 10 pages mixed with a 300 page scan | 600 | 310 | 16 | 0 | 0.000 | 0 | 0.000 |
| 4 hot set of 10 pages mixed with a 300 page scan | 600 | 310 | 64 | 290 | 0.483 | 279 | 0.465 |
| 4 hot set of 10 pages mixed with a 300 page scan | 600 | 310 | 256 | 290 | 0.483 | 283 | 0.472 |
| 5 nested loop: 20 outer pages, 40 page inner scan | 820 | 60 | 4 | 0 | 0.000 | 0 | 0.000 |
| 5 nested loop: 20 outer pages, 40 page inner scan | 820 | 60 | 16 | 0 | 0.000 | 0 | 0.000 |
| 5 nested loop: 20 outer pages, 40 page inner scan | 820 | 60 | 64 | 760 | 0.927 | 760 | 0.927 |
| 5 nested loop: 20 outer pages, 40 page inner scan | 820 | 60 | 256 | 760 | 0.927 | 760 | 0.927 |

The upper bound for any policy is 1 − distinct / accesses, because each distinct page misses once. The
pool reaches that bound in every row where B is at least the number of distinct pages: 801, 906, 720,
290 and 760 hits.

**Trace 3 under LRU at B = 64: zero hits out of 800.** This is sequential flooding. The scan touches 80
pages in a cycle, and LRU always evicts the page whose next use is furthest away, because in a cycle
longer than the pool the least recently used page is exactly the next one the scan will ask for. So
every access misses, B = 64 does no better than B = 4, and only once the whole file fits (B = 256) does
the second pass hit. CLOCK does the same here. Every page gets its reference bit set when it is loaded
and nothing references it again before the hand comes round, so each sweep clears all 64 bits and evicts
in arrival order, which in a cycle is LRU order.

Trace 4 shows the cost to other work: ten hot pages would fit easily in 16 frames, but each is touched
only every twentieth access, and the 300 page scan pushes nineteen other pages through between two
touches, so at B = 16 not one of the hot accesses hits.

**Which I would ship: CLOCK.** On the four traces that are not pure scans, CLOCK is within 0.018 of LRU's
hit rate everywhere, and on trace 3 they are equally bad. CLOCK costs a reference bit and a hand, with
no ordering to keep on every hit. My LRU searches all B stamps on each miss, and a real one would need a
list update under a latch on every hit. Sequential flooding is not something either policy fixes, so I
would fix it outside the policy. A query that knows it is scanning a table larger than the pool should
read through a small ring of frames of its own, which is what PostgreSQL does for large sequential
scans, or use LRU-K (O'Neil et al.), which can tell a page referenced once by a scan from one referenced
repeatedly.

## 3 Scan cost

`Big(a INT, b VARCHAR(20), c INT)` loaded with 120000 rows of 13 bytes: 500 data pages at 240 per page,
plus one directory page. Each scan is a fresh process: the pool starts empty after reading the catalog.

| B | hits | misses | evictions | reads | writes |
|---|---|---|---|---|---|
| 2 | 0 | 501 | 499 | 501 | 0 |
| 8 | 0 | 501 | 493 | 501 | 0 |
| 64 | 0 | 501 | 437 | 501 | 0 |

Page reads are 501 at every B: the 500 data pages and the directory page, each read once. The number
neither falls nor rises with B. It cannot fall, because nothing is cached between commands, and within
a scan no page is asked for twice. It does not rise because the scan never needs a page back once it
has unpinned it (section 4 of DESIGN_M1.md). Evictions are 501 − B: the first B misses fill empty
frames, and every miss after that replaces a page. B = 2 works even though only one frame is ever in use,
because the scan pins one page at a time.

## 4 Sources

Ramakrishnan and Gehrke chapter 9, for the explanation of sequential flooding in section 2. O'Neil,
O'Neil and Weikum, *The LRU-K Page Replacement Algorithm* (SIGMOD 1993), for why recency alone cannot
protect the hot set of trace 4. What each changed in the design is in DESIGN_M1.md section 8.
