#!/bin/bash
# tests/run_raid1_tests.sh - repeatable tests for FileGuard RAID-1 (mirror)
#
# Usage (from the project folder):  ./tests/run_raid1_tests.sh
#
# The tests run a temporary COPY of ./shellforge, so the real data/ folder
# of the project is never touched. FileGuard keeps its disk images in a
# "data" folder next to the program, so the copy gets its own fresh folder.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/shellforge"

if [ ! -x "$BIN" ]; then
    echo "shellforge binary not found. Run 'make' first."
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp "$BIN" "$WORK/shellforge"
SF="$WORK/shellforge"
DATA="$WORK/data"

PASS=0
FAIL=0

# Run Shellforge with the given commands, one per argument.
run_sf() {
    {
        echo "debug off"
        for c in "$@"; do
            echo "$c"
        done
        echo "exit"
    } | timeout 20 "$SF" 2>&1
}

ok() {
    PASS=$((PASS + 1))
    echo "PASS: $1"
}

bad() {
    FAIL=$((FAIL + 1))
    echo "FAIL: $1"
    echo "      expected: $2"
    echo "      got:      $3"
}

# check_has NAME EXPECTED_TEXT OUTPUT : output contains the text somewhere
check_has() {
    if printf '%s\n' "$3" | grep -qF -- "$2"; then
        ok "$1"
    else
        bad "$1" "$2" "$(printf '%s\n' "$3" | tail -4 | cut -c1-100 | tr '\n' '|')"
    fi
}

# check_line NAME EXPECTED_LINE OUTPUT : output has a line exactly equal to it
check_line() {
    if printf '%s\n' "$3" | grep -qxF -- "$2"; then
        ok "$1"
    else
        bad "$1" "$2" "$(printf '%s\n' "$3" | tail -4 | cut -c1-100 | tr '\n' '|')"
    fi
}

# check_count NAME TEXT EXPECTED_COUNT OUTPUT : text appears on N lines
check_count() {
    local n
    n=$(printf '%s\n' "$4" | grep -cF -- "$2")
    if [ "$n" = "$3" ]; then
        ok "$1"
    else
        bad "$1" "$3 lines containing: $2" "$n lines"
    fi
}

# check_eq NAME EXPECTED ACTUAL
check_eq() {
    if [ "$2" = "$3" ]; then
        ok "$1"
    else
        bad "$1" "$2" "$3"
    fi
}

# same_data: 1st and 2nd mirror disks hold identical data blocks.
# "cmp -i 512" skips each file's 512-byte header (the headers differ
# because each one stores its own disk number).
same_data() {
    cmp -s -i 512 "$DATA/disk1.img" "$DATA/disk2.img"
    echo $?
}

echo "=============================================================="
echo " FileGuard RAID-1 (mirror) test run"
echo " Date    : $(date)"
echo " Kernel  : $(uname -r)"
echo " GCC     : $(gcc --version | head -1)"
echo " Binary  : $BIN"
echo "=============================================================="

echo
echo "--- init raid1: headers and capacity ---"
out=$(run_sf "fileguard init raid1" "fileguard status")
check_has "init raid1 announces RAID-1 (mirror)" "as RAID-1 (mirror)" "$out"
check_has "init completes" "Done. Run 'fileguard status'" "$out"
check_has "status shows the RAID-1 volume with 1024 blocks" \
    "Volume: RAID-1 mirror of Disk 1 and Disk 2, 1024 logical blocks (524288 bytes)" "$out"
check_has "mirror state is HEALTHY" "Mirror state: HEALTHY (2 of 2 copies available)" "$out"
check_has "status reports 4 of 4 disks online" "Disks online: 4 of 4" "$out"
for i in 1 2 3 4; do
    check_eq "disk$i.img is 524800 bytes" "524800" \
        "$(stat -c %s "$DATA/disk$i.img" 2>/dev/null)"
    check_eq "disk$i.img header: version 2 and RAID level 1" \
        "02 00 00 00 01 00 00 00" \
        "$({ od -An -tx1 -j4 -N4 "$DATA/disk$i.img"; od -An -tx1 -j24 -N4 "$DATA/disk$i.img"; } | xargs)"
done

echo
echo "--- Write and read back (every block exists twice) ---"
out=$(run_sf "fileguard write 5 \"mirror-5\"")
check_has "write reports both copies" \
    "Wrote 8 bytes to logical block 5 -> Disk 1 and Disk 2, block 5" "$out"
BLOCKS="0 1 2 3 4 5 100 1023"
cmds=()
for b in $BLOCKS; do
    cmds+=("fileguard write $b \"mirror-$b\"")
done
for b in $BLOCKS; do
    cmds+=("fileguard read $b")
done
out=$(run_sf "${cmds[@]}")
for b in $BLOCKS; do
    check_line "block $b reads back exactly what was written" "  mirror-$b" "$out"
done

echo
echo "--- Both copies are really on disk ---"
check_eq "block 5 text is on Disk 1 (file offset 3072)" "mirror-5" \
    "$(dd if="$DATA/disk1.img" bs=1 skip=3072 count=8 2>/dev/null)"
check_eq "block 5 text is on Disk 2 (file offset 3072)" "mirror-5" \
    "$(dd if="$DATA/disk2.img" bs=1 skip=3072 count=8 2>/dev/null)"
check_eq "data areas of Disk 1 and Disk 2 are identical" "0" "$(same_data)"
cmp -s -i 512 -n 524288 "$DATA/disk3.img" /dev/zero
check_eq "Disk 3 holds no data (unused)" "0" "$?"
cmp -s -i 512 -n 524288 "$DATA/disk4.img" /dev/zero
check_eq "Disk 4 holds no data (unused)" "0" "$?"

echo
echo "--- Capacity is 1024 blocks ---"
row=$(printf '%13u   %-15s   %10u' 1023 "Disk 1 + Disk 2" 1023)
out=$(run_sf "fileguard write 1023 \"last\"" "fileguard write 1024 \"x\"" \
             "fileguard read 1024" "fileguard read 2000" \
             "fileguard layout 1023" "fileguard layout 1024")
check_has "the last block (1023) can be written" "Wrote 4 bytes to logical block 1023" "$out"
check_count "4 out-of-range block numbers are rejected (1024, 2000)" \
    "Error: block must be a number from 0 to 1023" 4 "$out"
check_line "layout shows block 1023 on both mirror disks" "$row" "$out"
out=$(run_sf "fileguard layout")
for b in 0 3 7; do
    row=$(printf '%13u   %-15s   %10u' "$b" "Disk 1 + Disk 2" "$b")
    check_line "layout row for block $b" "$row" "$out"
done

echo
echo "--- Overwrite with shorter text clears both copies ---"
run_sf "fileguard write 7 \"first long text\"" "fileguard write 7 \"second\"" > /dev/null
out=$(run_sf "fileguard read 7")
check_line "overwritten block reads back the new text" "  second" "$out"
check_eq "Disk 1: old text fully cleared" "00 00 00 00 00 00 00 00" \
    "$(od -An -tx1 -j 4103 -N 8 "$DATA/disk1.img" | xargs)"
check_eq "Disk 2: old text fully cleared" "00 00 00 00 00 00 00 00" \
    "$(od -An -tx1 -j 4103 -N 8 "$DATA/disk2.img" | xargs)"
check_eq "copies still identical after the overwrite" "0" "$(same_data)"

echo
echo "--- Disk 1 missing: volume keeps working (degraded) ---"
run_sf "fileguard init raid1" "fileguard write 0 \"copy-one\"" \
       "fileguard write 1 \"copy-two\"" > /dev/null
rm -f "$DATA/disk1.img"
out=$(run_sf "fileguard status" "fileguard read 0" "fileguard read 1" \
             "fileguard write 2 \"while degraded\"" "fileguard read 2" \
             "echo ALIVE-OK")
check_has "status marks the removed disk MISSING" "MISSING" "$out"
check_has "mirror state is DEGRADED" "Mirror state: DEGRADED (1 of 2 copies available" "$out"
check_line "block 0 is still readable from Disk 2" "  copy-one" "$out"
check_line "block 1 is still readable from Disk 2" "  copy-two" "$out"
check_has "read says it was served from Disk 2" \
    "(DEGRADED: Disk 1 unavailable, data served from Disk 2)" "$out"
check_has "write while degraded reports one copy only" \
    "Wrote 14 bytes to logical block 2 -> Disk 2 only, block 2 (DEGRADED: Disk 1 unavailable)" "$out"
check_line "data written while degraded can be read back" "  while degraded" "$out"
check_eq "data written while degraded is on Disk 2" "while degraded" \
    "$(dd if="$DATA/disk2.img" bs=1 skip=1536 count=14 2>/dev/null)"
check_line "shell is still alive" "ALIVE-OK" "$out"

echo
echo "--- Disk 2 missing: volume keeps working (degraded) ---"
run_sf "fileguard init raid1" "fileguard write 0 \"from-disk-one\"" > /dev/null
rm -f "$DATA/disk2.img"
out=$(run_sf "fileguard status" "fileguard read 0" \
             "fileguard write 1 \"only-disk-one\"" "fileguard read 1")
check_has "mirror state is DEGRADED" "Mirror state: DEGRADED (1 of 2 copies available" "$out"
check_line "old data is readable from Disk 1" "  from-disk-one" "$out"
check_has "write reports Disk 1 only" \
    "Wrote 13 bytes to logical block 1 -> Disk 1 only, block 1 (DEGRADED: Disk 2 unavailable)" "$out"
check_line "new data is readable from Disk 1" "  only-disk-one" "$out"

echo
echo "--- Disk 1 header damaged (CORRUPT counts as failed) ---"
run_sf "fileguard init raid1" "fileguard write 0 \"survivor\"" > /dev/null
dd if=/dev/zero of="$DATA/disk1.img" bs=1 count=16 conv=notrunc 2>/dev/null
out=$(run_sf "fileguard status" "fileguard read 0")
check_has "damaged header is reported CORRUPT" "CORRUPT" "$out"
check_has "mirror state is DEGRADED" "Mirror state: DEGRADED (1 of 2 copies available" "$out"
check_line "data is still readable" "  survivor" "$out"
check_has "read was served from Disk 2" "data served from Disk 2" "$out"

echo
echo "--- Both mirror disks lost ---"
run_sf "fileguard init raid1" "fileguard write 0 \"doomed\"" > /dev/null
rm -f "$DATA/disk1.img" "$DATA/disk2.img"
out=$(run_sf "fileguard status" "fileguard read 0" "fileguard write 0 \"x\"" \
             "echo ALIVE-OK")
check_has "mirror state is FAILED" \
    "Mirror state: FAILED (0 of 2 copies available, data unavailable)" "$out"
check_has "status counts 2 of 4 disks online" "Disks online: 2 of 4" "$out"
check_has "read fails with a clear message" "Error: read failed" "$out"
check_has "write fails with a clear message" "Error: write failed" "$out"
check_line "shell is still alive" "ALIVE-OK" "$out"

echo
echo "--- Persistence across shell restarts ---"
run_sf "fileguard init raid1" "fileguard write 42 \"persist me\"" > /dev/null
out=$(run_sf "fileguard read 42" "fileguard status")
check_line "data written in one session is read in a new session" "  persist me" "$out"
check_has "the RAID level is remembered in a new session" \
    "Volume: RAID-1 mirror of Disk 1 and Disk 2" "$out"

echo
echo "--- Switching RAID levels and bad init arguments ---"
run_sf "fileguard init raid1" "fileguard write 9 \"keep me\"" > /dev/null
out=$(run_sf "fileguard init raid9" "fileguard init raid1 extra" \
             "fileguard read 9" "echo ALIVE-OK")
check_has "unknown RAID level is rejected" \
    "Error: unknown RAID level 'raid9' (use raid0, raid1 or raid5)" "$out"
check_has "extra init argument shows usage" "Usage: fileguard init [raid0|raid1|raid5]" "$out"
check_line "rejected init commands did not erase the data" "  keep me" "$out"
check_line "shell is still alive" "ALIVE-OK" "$out"
out=$(run_sf "fileguard init" "fileguard status" \
             "fileguard write 4095 \"raid0-last\"" "fileguard read 4095")
check_has "plain 'init' still creates RAID-0" "as RAID-0 (striping)" "$out"
check_has "status shows the RAID-0 volume with 4096 blocks" \
    "Volume: RAID-0 striping, 4096 logical blocks" "$out"
check_line "block 4095 works again in RAID-0" "  raid0-last" "$out"
out=$(run_sf "fileguard init raid1" "fileguard read 4095")
check_has "back in RAID-1 block 4095 is out of range again" \
    "Error: block must be a number from 0 to 1023" "$out"

echo
echo "=============================================================="
echo " Passed: $PASS    Failed: $FAIL"
echo "=============================================================="

if [ "$FAIL" -eq 0 ]; then
    exit 0
fi
exit 1
