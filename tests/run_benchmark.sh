#!/bin/bash
# FileGuard benchmark: write/read N blocks per RAID level, plus raw single-file dd baseline.
cd "$(dirname "$0")/.." || exit 1
N=${1:-500}
OUT=tests/results/benchmark_run1.txt
now() { date +%s.%N; }
{
echo "FileGuard benchmark  $(date)  blocks per test: $N (512 bytes each)"
echo "Note: runs through the Shellforge shell, so parsing overhead is included."
printf "%-8s %12s %12s\n" LEVEL "write(s)" "read(s)"
for lvl in raid0 raid1 raid5; do
  { echo "debug off"; echo "fileguard init $lvl"; echo exit; } | ./shellforge >/dev/null 2>&1
  s=$(now)
  { echo "debug off"; for i in $(seq 0 $((N-1))); do echo "fileguard write $i \"benchmark block $i\""; done; echo exit; } | ./shellforge >/dev/null 2>&1
  m=$(now)
  { echo "debug off"; for i in $(seq 0 $((N-1))); do echo "fileguard read $i"; done; echo exit; } | ./shellforge >/dev/null 2>&1
  e=$(now)
  printf "%-8s %12.3f %12.3f\n" "$lvl" "$(echo "$m - $s" | bc -l)" "$(echo "$e - $m" | bc -l)"
done
s=$(now); dd if=/dev/zero of=/tmp/single.img bs=512 count=$N status=none; m=$(now)
dd if=/tmp/single.img of=/dev/null bs=512 count=$N status=none; e=$(now)
printf "%-8s %12.3f %12.3f   (raw single file, no shell)\n" single "$(echo "$m - $s" | bc -l)" "$(echo "$e - $m" | bc -l)"
rm -f /tmp/single.img
} | tee "$OUT"
