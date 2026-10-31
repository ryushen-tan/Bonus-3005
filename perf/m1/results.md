## Policies

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

## Capacity

| schema | record bytes | computed per page | measured on page 1 | measured tuples / data pages |
|---|---|---|---|---|
| a:INT,b:INT,c:INT | 12 | floor(4088 / 16) = 255 | 255 | 5000 / 20 = 250.00 |
| a:INT,b:VARCHAR(20),c:INT | 13 | floor(4088 / 17) = 240 | 240 | 5000 / 21 = 238.10 |
| a:INT,b:VARCHAR(200) | 206 | floor(4088 / 210) = 19 | 19 | 5000 / 264 = 18.94 |

## Scan cost

loaded 120000 rows into Big, now 120000 tuples on 500 data pages

| B | hits | misses | evictions | reads | writes |
|---|---|---|---|---|---|
| 2 | 0 | 501 | 499 | 501 | 0 |
| 8 | 0 | 501 | 493 | 501 | 0 |
| 64 | 0 | 501 | 437 | 501 | 0 |
