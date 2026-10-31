## 1 Cost against formula

Data pages M and N exclude the directory page each scan also reads. Measured is the plan total: the two scans plus the join's own reads and writes.

| join | tuples per side | M = N pages | B | measured reads | measured writes | measured total | formula | measured / formula | output |
|---|---|---|---|---|---|---|---|---|---|
| BNL | 1500 | 20 | 4 | 231 | 0 | 231 | 220 | 1.05 | 1500 |
| BNL | 1500 | 20 | 16 | 63 | 0 | 63 | 60 | 1.05 | 1500 |
| BNL | 1500 | 20 | 64 | 42 | 0 | 42 | 40 | 1.05 | 1500 |
| BNL | 1500 | 20 | 256 | 42 | 0 | 42 | 40 | 1.05 | 1500 |
| SMJ | 1500 | 20 | 4 | 162 | 120 | 282 | 240 | 1.18 | 1500 |
| SMJ | 1500 | 20 | 16 | 122 | 80 | 202 | 160 | 1.26 | 1500 |
| SMJ | 1500 | 20 | 64 | 82 | 40 | 122 | 80 | 1.52 | 1500 |
| SMJ | 1500 | 20 | 256 | 82 | 40 | 122 | 80 | 1.52 | 1500 |
| HJ | 1500 | 20 | 4 | 152 | 44 | 196 | 120 | 1.63 | 1500 |
| HJ | 1500 | 20 | 16 | 100 | 58 | 158 | 120 | 1.32 | 1500 |
| HJ | 1500 | 20 | 64 | 168 | 126 | 294 | 120 | 2.45 | 1500 |
| HJ | 1500 | 20 | 256 | 550 | 508 | 1058 | 120 | 8.82 | 1500 |
| BNL | 6000 | 80 | 4 | 3321 | 0 | 3321 | 3280 | 1.01 | 6000 |
| BNL | 6000 | 80 | 16 | 567 | 0 | 567 | 560 | 1.01 | 6000 |
| BNL | 6000 | 80 | 64 | 243 | 0 | 243 | 240 | 1.01 | 6000 |
| BNL | 6000 | 80 | 256 | 162 | 0 | 162 | 160 | 1.01 | 6000 |
| SMJ | 6000 | 80 | 4 | 786 | 624 | 1410 | 1280 | 1.10 | 6000 |
| SMJ | 6000 | 80 | 16 | 482 | 320 | 802 | 640 | 1.25 | 6000 |
| SMJ | 6000 | 80 | 64 | 482 | 320 | 802 | 640 | 1.25 | 6000 |
| SMJ | 6000 | 80 | 256 | 322 | 160 | 482 | 320 | 1.51 | 6000 |
| HJ | 6000 | 80 | 4 | 1395 | 164 | 1559 | 480 | 3.25 | 6000 |
| HJ | 6000 | 80 | 16 | 338 | 176 | 514 | 480 | 1.07 | 6000 |
| HJ | 6000 | 80 | 64 | 414 | 252 | 666 | 480 | 1.39 | 6000 |
| HJ | 6000 | 80 | 256 | 672 | 510 | 1182 | 480 | 2.46 | 6000 |
| BNL | 24000 | 320 | 4 | 51681 | 0 | 51681 | 51520 | 1.00 | 24000 |
| BNL | 24000 | 320 | 16 | 7704 | 0 | 7704 | 7680 | 1.00 | 24000 |
| BNL | 24000 | 320 | 64 | 2247 | 0 | 2247 | 2240 | 1.00 | 24000 |
| BNL | 24000 | 320 | 256 | 963 | 0 | 963 | 960 | 1.00 | 24000 |
| SMJ | 24000 | 320 | 4 | 3842 | 3200 | 7042 | 6400 | 1.10 | 24000 |
| SMJ | 24000 | 320 | 16 | 2562 | 1920 | 4482 | 3840 | 1.17 | 24000 |
| SMJ | 24000 | 320 | 64 | 1922 | 1280 | 3202 | 2560 | 1.25 | 24000 |
| SMJ | 24000 | 320 | 256 | 1922 | 1280 | 3202 | 2560 | 1.25 | 24000 |
| HJ | 24000 | 320 | 4 | 18196 | 642 | 18838 | 1920 | 9.81 | 24000 |
| HJ | 24000 | 320 | 16 | 1657 | 656 | 2313 | 1920 | 1.20 | 24000 |
| HJ | 24000 | 320 | 64 | 1354 | 712 | 2066 | 1920 | 1.08 | 24000 |
| HJ | 24000 | 320 | 256 | 1638 | 996 | 2634 | 1920 | 1.37 | 24000 |

## 2 Sort passes

| N pages | B | runs after pass 0 | measured passes | 1 + ceil(log(ceil(N/B)) / log(B-1)) | sort reads | sort writes | scan reads |
|---|---|---|---|---|---|---|---|
| 20 | 4 | 5 | 3 | 3 | 40 | 40 | 21 |
| 80 | 4 | 20 | 4 | 4 | 232 | 232 | 81 |
| 80 | 8 | 10 | 3 | 3 | 160 | 160 | 81 |
| 320 | 16 | 20 | 3 | 3 | 640 | 640 | 321 |
| 320 | 4 | 80 | 5 | 5 | 1280 | 1280 | 321 |
| 320 | 64 | 5 | 2 | 2 | 320 | 320 | 321 |

## 3 Crossover

Varying B with R6000 and S6000 fixed (M = N = 80 pages):

| B | BNL I/O | SMJ I/O | HJ I/O | cheapest |
|---|---|---|---|---|
| 4 | 3321 | 1410 | 1559 | SMJ |
| 6 | 1701 | 1122 | 763 | HJ |
| 8 | 1215 | 1122 | 582 | HJ |
| 12 | 729 | 802 | 502 | HJ |
| 16 | 567 | 802 | 514 | HJ |
| 24 | 405 | 802 | 530 | BNL |
| 32 | 324 | 802 | 534 | BNL |
| 48 | 243 | 802 | 538 | BNL |
| 64 | 243 | 802 | 666 | BNL |
| 96 | 162 | 482 | 582 | BNL |
| 128 | 162 | 482 | 670 | BNL |

Varying the size of both relations with B = 16 fixed:

| tuples per side | M = N pages | BNL I/O | SMJ I/O | HJ I/O | cheapest |
|---|---|---|---|---|---|
| 300 | 4 | 10 | 26 | 70 | BNL |
| 600 | 8 | 18 | 50 | 82 | BNL |
| 1200 | 16 | 51 | 98 | 126 | BNL |
| 2400 | 32 | 132 | 322 | 222 | BNL |
| 4800 | 64 | 390 | 642 | 410 | BNL |
| 9600 | 128 | 1419 | 1282 | 818 | HJ |

## 4 Skew

| data | B | partitions, pages left/right | overflowed | reads | writes | comparisons | output |
|---|---|---|---|---|---|---|---|
| uniform | 16 | [5/5,6/6,5/5,5/5,5/5,7/7,6/6,5/5,5/5,6/6,5/5,5/5,7/7,10/10,6/6] | 0 | 338 | 176 | 15582 | 6000 |
| skewed | 16 | [3/3,35/35,3/3,3/3,3/3,4/4,4/4,3/3,3/3,4/4,3/3,3/3,4/4,6/6,4/4] | 1 | 402 | 170 | 6882580 | 5768400 |

## 5 The budget

Sort of Twenty, 82 data pages, at B = 4 (20 x B = 80): 6600 tuples out, peak frames 4.

```
sort tuples_out=6600 comparisons=82556 reads=236 writes=236 passes=4
  seqscan Twenty tuples_out=6600 comparisons=0 reads=83 writes=0
```

## 6 What materialising an input costs

A hash group_by over a hash join: the join holds memory, so the group_by writes its output to a temporary file before taking memory itself.

B = 8, peak frames 8:

```
group_by hash tuples_out=6000 comparisons=15421 reads=214 writes=168 levels=1 partitions=7 spilled_pages=[2,2,2,2,2,2,2]
  hash_join tuples_out=6000 comparisons=6009418 reads=332 writes=166 partitions=6 pages_left/right=[13/13,14/14,14/14,14/14,15/15,13/13] overflowed=6
    seqscan R6000 tuples_out=6000 comparisons=0 reads=81 writes=0
    seqscan S6000 tuples_out=6000 comparisons=0 reads=81 writes=0
```

B = 16, peak frames 16:

```
group_by hash tuples_out=6000 comparisons=14856 reads=268 writes=169 levels=1 partitions=15 spilled_pages=[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]
  hash_join tuples_out=6000 comparisons=25320 reads=168 writes=168 partitions=14 pages_left/right=[6/6,6/6,6/6,6/6,6/6,6/6,6/6,6/6,6/6,6/6,6/6,6/6,6/6,6/6] overflowed=0
    seqscan R6000 tuples_out=6000 comparisons=0 reads=81 writes=0
    seqscan S6000 tuples_out=6000 comparisons=0 reads=81 writes=0
```

## 7 Hash distinct against its formula

A hash distinct of R, whose tuples are all different, so the groups take as many pages as the input. The scan's reads, which include the input read again when a level partitions, the distinct's own reads and writes, and the formula of DESIGN_M2.md section 6.

| N pages | B | levels | partitions per split | scan reads | own reads | own writes | total I/O | formula | measured / formula |
|---|---|---|---|---|---|---|---|---|---|
| 80 | 4 | 4 | 3 | 86 | 405 | 313 | 804 | 617 | 1.30 |
| 80 | 8 | 2 | 7 | 90 | 238 | 182 | 510 | 507 | 1.01 |
| 80 | 16 | 1 | 15 | 98 | 89 | 89 | 276 | 275 | 1.00 |
| 80 | 64 | 1 | 63 | 146 | 125 | 125 | 396 | 395 | 1.00 |
| 80 | 256 | 0 | 255 | 81 | 0 | 0 | 81 | 80 | 1.01 |
| 320 | 4 | 5 | 3 | 326 | 2294 | 1814 | 4434 | 4361 | 1.02 |
| 320 | 8 | 3 | 7 | 330 | 775 | 695 | 1800 | 1713 | 1.05 |
| 320 | 16 | 2 | 15 | 338 | 1016 | 776 | 2130 | 2135 | 1.00 |
| 320 | 64 | 1 | 63 | 386 | 350 | 350 | 1086 | 1139 | 0.95 |
| 320 | 256 | 1 | 255 | 578 | 503 | 503 | 1584 | 1595 | 0.99 |
