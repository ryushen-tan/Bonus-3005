# REPORT_M1.md: storage measurements

Machine: Apple M5 Pro, 24 GB, macOS 26.5.2, Apple clang 21.0.0, `-std=c++17 -O2`. Every number below comes
from `python3 perf/m1.py`. That script writes the traces and the data, runs `./storage`, and prints these
tables into `perf/m1/results.md`. It takes under a second. I ran it twice and the second run printed
identical numbers.

The five traces the handout publishes weren't on Brightspace when I did this. So `perf/m1.py` writes five
of its own with a fixed linear congruential generator, the example `rand` of the C standard. Like that
example, it keeps only the high 15 bits of each value. The low bits of a generator modulo a power of two
repeat with short periods, and the lowest one just alternates. With two draws per access, the second one
would always be even. Trace 3 is the one the handout describes, a sequential scan of an 80 page file
repeated ten times. The traces are in `perf/m1/traces/`, and `storage trace` runs any trace file, so the
published ones can be dropped in.

## 1 Capacity

This is the number of records per page under my layout (section 3.3 of the module). A record of L bytes
needs L bytes of data and a 4 byte slot. A page has 4096 − 8 = 4088 bytes after the header, so the
capacity is floor(4088 / (L + 4)). I loaded 5000 rows into each table, and `storage layout --page 1` gives
the measured count on the first data page.

| schema | record bytes L | computed per page | measured on page 1 | 5000 tuples / data pages |
|---|---|---|---|---|
| `a:INT,b:INT,c:INT` | 12 | floor(4088 / 16) = 255 | 255 | 5000 / 20 = 250.00 |
| `a:INT,b:VARCHAR(20),c:INT`, b = `bob` | 13 | floor(4088 / 17) = 240 | 240 | 5000 / 21 = 238.10 |
| `a:INT,b:VARCHAR(200)`, b 200 characters | 206 | floor(4088 / 210) = 19 | 19 | 5000 / 264 = 18.94 |

The computed and measured values per page agree exactly. A full page also wastes whatever is left after the
last record: 4088 − 255 × 16 = 8 bytes, 4088 − 240 × 17 = 8 bytes, and 4088 − 19 × 210 = 98 bytes. The 98
bytes are almost half a record of the long schema. 19 × 210 = 3990, and a twentieth record would need
4200.

The only gap is in the last column, and it isn't caused by the layout. 5000 isn't a multiple of the
capacity, so the last page is only part full. There are 19 full pages of 255 plus one page of 155, 20
pages of 240 plus one of 200, and 263 pages of 19 plus one of 3. This is also where the 1 in the "tuples
per page" estimates of Module 4 comes from. A VARCHAR column whose values vary in length would also make
the count per page vary from page to page. That's because the capacity depends on the actual bytes, not
the declared n. A VARCHAR(200) column of 10 character strings packs like a 16 byte record, not a 206 byte
one.

## 2 Replacement policies

I ran each trace under LRU and CLOCK with B = 4, 16, 64 and 256. The pool started empty, and every page
was unpinned clean right after it was fetched. Hit rate is hits divided by accesses.

| trace | accesses | distinct pages | B | LRU hits | LRU hit rate | CLOCK hits | CLOCK hit rate |
|---|---|---|---|---|---|---|---|
| 1 uniform over 200 pages | 1000 | 198 | 4 | 20 | 0.020 | 21 | 0.021 |
| 1 uniform over 200 pages | 1000 | 198 | 16 | 73 | 0.073 | 71 | 0.071 |
| 1 uniform over 200 pages | 1000 | 198 | 64 | 297 | 0.297 | 296 | 0.296 |
| 1 uniform over 200 pages | 1000 | 198 | 256 | 802 | 0.802 | 802 | 0.802 |
| 2 80/20 skew over 200 pages | 1000 | 158 | 4 | 50 | 0.050 | 49 | 0.049 |
| 2 80/20 skew over 200 pages | 1000 | 158 | 16 | 243 | 0.243 | 234 | 0.234 |
| 2 80/20 skew over 200 pages | 1000 | 158 | 64 | 745 | 0.745 | 715 | 0.715 |
| 2 80/20 skew over 200 pages | 1000 | 158 | 256 | 842 | 0.842 | 842 | 0.842 |
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

The upper bound for any policy is 1 − distinct / accesses, because each distinct page misses once. Both
policies reach that bound in every row where B is at least the number of distinct pages. That's 802, 842
and 720 hits on traces 1 to 3 at B = 256, and 760 on trace 5 at B = 64 and 256. Trace 4 has 310 distinct
pages, which is more than any B here. But only its ten hot pages are ever asked for twice, so its bound of
290 hits only needs the hot set kept in the pool. LRU keeps it from B = 64 on and reaches 290. CLOCK gets
279 and 283.

Trace 3 under LRU at B = 64 gets zero hits out of 800. This is sequential flooding. The scan touches 80
pages in a cycle. When the cycle is longer than the pool, the least recently used page is exactly the
next one the scan will ask for. So LRU always evicts the page it's about to need, and every access
misses. B = 64 does no better than B = 4, and the second pass only hits once the whole file fits (B =
256). CLOCK does the same here. Every page gets its reference bit set when it is loaded, and nothing
references it again before the hand comes round. So each sweep clears all 64 bits and evicts in arrival
order, which in a cycle is LRU order.

Trace 4 shows what this costs other work. Ten hot pages would fit easily in 16 frames. But each one is
only touched every twentieth access, and the 300 page scan pushes nineteen other pages through between two
touches. So at B = 16, not one of the hot accesses hits.

I would ship CLOCK, and it is the policy the tools use when `--policy` isn't given. On the four traces
that aren't pure scans, CLOCK is within 0.030 of LRU's hit rate everywhere. The widest gap is trace 2 at B
= 64 (0.715 against 0.745). On trace 3 they are equally bad. CLOCK costs a reference bit and a hand, with
no ordering to keep up on every hit. My LRU searches all B stamps on each miss, and a real one would need
a list update under a latch on every hit. Neither policy fixes sequential flooding, so I would fix it
outside the policy. A query that knows it is scanning a table larger than the pool should read through a
small ring of frames of its own, which is what PostgreSQL does for large sequential scans. Or it could use
LRU-K (O'Neil et al.), which can tell a page referenced once by a scan from one referenced repeatedly.

## 3 Scan cost

`Big(a INT, b VARCHAR(20), c INT)` was loaded with 120000 rows of 13 bytes. That is 500 data pages at 240
per page, plus one directory page. Each scan is a fresh process, so the pool starts empty after reading
the catalog. The scans ran under the default policy, CLOCK. `--policy lru` gives the same counters at
every B, because the scan never asks for a page twice.

| B | hits | misses | evictions | reads | writes |
|---|---|---|---|---|---|
| 2 | 0 | 501 | 499 | 501 | 0 |
| 8 | 0 | 501 | 493 | 501 | 0 |
| 64 | 0 | 501 | 437 | 501 | 0 |

Page reads are 501 at every B. That is the 500 data pages and the directory page, each read once. The
number doesn't fall or rise with B. It can't fall, because nothing is cached between commands, and within
a scan no page is asked for twice. It doesn't rise because the scan never needs a page back once it has
unpinned it (section 4 of DESIGN_M1.md). Evictions are 501 − B. The first B misses fill empty frames, and
every miss after that replaces a page. So at B = 2 both frames hold pages. But only one of them is ever
pinned, because the scan pins one page at a time. That is why B = 2 works, and B = 1 does too (501 reads
and 500 evictions).

## 4 Sources

Ramakrishnan and Gehrke chapter 9, for the explanation of sequential flooding in section 2. O'Neil,
O'Neil and Weikum, *The LRU-K Page Replacement Algorithm* (SIGMOD 1993), for why recency alone can't
protect the hot set of trace 4. What each one changed in the design is in DESIGN_M1.md section 8.
