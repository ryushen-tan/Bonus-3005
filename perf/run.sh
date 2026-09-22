#!/bin/sh
# runs the experiment and writes a tsv to stdout, each query runs 3 times and the fastest is kept
set -e
cd "$(dirname "$0")/.."
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
printf 'query\tn\tm\tk\tcomparisons\ttuples\ttime\n'
row() {
  echo "$1  n=$2 m=$3 k=$4" >&2
  s=$(for i in 1 2 3; do ./ra --stats "$D/d.txt" "$1"; done | sort -t= -k4 -g | head -1 | sed 's/[a-z]*=//g' | tr ' ' '\t')
  printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$s"
}
for n in 1000 2000 4000 8000 16000 32000 64000; do
  ./gen $n $n 1 > "$D/d.txt"
  for q in 'R join[R.b=S.b] S' 'select[a>500](R)' 'project[b](R)'; do row "$q" $n $n 1; done
done
for nm in '1000 64000' '64000 1000'; do
  ./gen $nm 1 > "$D/d.txt"
  row 'R join[R.b=S.b] S' $nm 1
done
for k in 0 1 10 100; do
  ./gen 16000 16000 $k > "$D/d.txt"
  for q in 'R join[R.b=S.b] S' 'project[b](R)'; do row "$q" 16000 16000 $k; done
done
