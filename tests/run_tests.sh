#!/bin/bash
# tests/run_tests.sh - repeatable tests for FileGuard (RAID-0) inside Shellforge
#
# Usage (from the project folder):  ./tests/run_tests.sh
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
# "debug off" first hides the TOKENS/PIPELINE tables, "exit" ends the session,
# and timeout makes sure a stuck shell can never freeze the test run.
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
        bad "$1" "$2" "$(printf '%s\n' "$3" | tail -4 | tr '\n' '|')"
    fi
}

# check_line NAME EXPECTED_LINE OUTPUT : output has a line exactly equal to it
check_line() {
    if printf '%s\n' "$3" | grep -qxF -- "$2"; then
        ok "$1"
    else
        bad "$1" "$2" "$(printf '%s\n' "$3" | tail -4 | tr '\n' '|')"
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

echo "=============================================================="
echo " FileGuard RAID-0 test run"
echo " Date    : $(date)"
echo " Kernel  : $(uname -r)"
echo " GCC     : $(gcc --version | head -1)"
echo " Binary  : $BIN"
echo "=============================================================="

echo
echo "--- Before init ---"
out=$(run_sf "fileguard status" "fileguard read 0")
check_has "status before init reports 0 of 4 disks online" "Disks online: 0 of 4" "$out"
check_has "read before init fails with a clear message" "Error: read failed" "$out"

echo
echo "--- init and disk geometry ---"
out=$(run_sf "fileguard init" "fileguard status")
check_has "init completes" "Done. Run 'fileguard status'" "$out"
check_has "status reports 4 of 4 disks online" "Disks online: 4 of 4" "$out"
for i in 1 2 3 4; do
    check_eq "disk$i.img is 524800 bytes" "524800" \
        "$(stat -c %s "$DATA/disk$i.img" 2>/dev/null)"
    check_eq "disk$i.img starts with the FileGuard magic bytes" "44 52 47 46" \
        "$(od -An -tx1 -N4 "$DATA/disk$i.img" 2>/dev/null | xargs)"
done

echo
echo "--- Round trip: write, then read back ---"
BLOCKS="0 1 2 3 4 5 6 7 100 4095"
cmds=()
for b in $BLOCKS; do
    cmds+=("fileguard write $b \"data-$b\"")
done
for b in $BLOCKS; do
    cmds+=("fileguard read $b")
done
out=$(run_sf "${cmds[@]}")
for b in $BLOCKS; do
    check_line "block $b reads back exactly what was written" "  data-$b" "$out"
done

echo
echo "--- Logical block to disk mapping ---"
MAPBLOCKS="0 1 2 3 4 5 6 7 4095"
cmds=()
for b in $MAPBLOCKS; do
    cmds+=("fileguard layout $b")
done
out=$(run_sf "${cmds[@]}")
for b in $MAPBLOCKS; do
    d=$((b % 4 + 1))
    k=$((b / 4))
    row=$(printf '%13u   Disk %d   %10u' "$b" "$d" "$k")
    check_line "block $b maps to Disk $d, disk block $k" "$row" "$out"
done

echo
echo "--- Bytes inside the disk image files ---"
run_sf "fileguard write 0 \"ABC\"" "fileguard write 5 \"Block five\"" \
       "fileguard write 4 \"data-4\"" > /dev/null
check_eq "block 0: 'ABC' plus end-of-string at disk1.img offset 512" "41 42 43 00" \
    "$(od -An -tx1 -j 512 -N 4 "$DATA/disk1.img" | xargs)"
check_eq "block 5: text at disk2.img offset 1024 (disk 2, disk block 1)" "Block five" \
    "$(dd if="$DATA/disk2.img" bs=1 skip=1024 count=10 2>/dev/null)"
check_eq "block 4: text at disk1.img offset 1024 (striping wraps to disk 1)" "data-4" \
    "$(dd if="$DATA/disk1.img" bs=1 skip=1024 count=6 2>/dev/null)"

echo
echo "--- Overwrite with shorter text ---"
run_sf "fileguard write 7 \"first long text\"" "fileguard write 7 \"second\"" > /dev/null
out=$(run_sf "fileguard read 7")
check_line "overwritten block reads back the new text" "  second" "$out"
# block 7 = disk 4, disk block 1 -> file offset 1024. "second"+NUL fills
# bytes 1024-1030, so bytes 1031-1038 must be zero padding again.
check_eq "old text is fully cleared (zero padding)" "00 00 00 00 00 00 00 00" \
    "$(od -An -tx1 -j 1031 -N 8 "$DATA/disk4.img" | xargs)"

echo
echo "--- Text length limits ---"
t511=$(head -c 511 /dev/zero | tr '\0' 'x')
t512=$(head -c 512 /dev/zero | tr '\0' 'x')
out=$(run_sf "fileguard write 20 \"$t511\"" "fileguard write 21 \"$t512\"")
check_has "511 characters are accepted" "Wrote 511 bytes to logical block 20" "$out"
check_has "512 characters are rejected" "Error: text too long (maximum 511 characters)" "$out"

echo
echo "--- Invalid input (shell must stay alive) ---"
out=$(run_sf "fileguard read 4096" "fileguard read -1" "fileguard read abc" \
             "fileguard write 4096 \"x\"" "fileguard write 0 Hello World" \
             "fileguard read" "fileguard foo" "fileguard" "echo ALIVE-OK")
check_count "4 bad block numbers are rejected (4096, -1, abc, write 4096)" \
    "Error: block must be a number from 0 to 4095" 4 "$out"
check_has "write without quotes shows usage" "Usage: fileguard write" "$out"
check_has "read without a block shows usage" "Usage: fileguard read" "$out"
check_has "unknown sub-command is reported" "Unknown command: foo" "$out"
check_has "plain 'fileguard' shows the usage text" \
    "FileGuard - user-space RAID-based virtual storage system" "$out"
check_line "shell is still alive after all the errors" "ALIVE-OK" "$out"

echo
echo "--- Persistence across shell restarts ---"
run_sf "fileguard write 42 \"persist me\"" > /dev/null
out=$(run_sf "fileguard read 42")
check_line "data written in one session is read in a new session" "  persist me" "$out"

echo
echo "--- Missing disk (RAID-0 has no redundancy) ---"
run_sf "fileguard write 0 \"on disk one\"" "fileguard write 1 \"on disk two\"" > /dev/null
rm -f "$DATA/disk2.img"
out=$(run_sf "fileguard status" "fileguard read 0" "fileguard read 1" "echo ALIVE-OK")
check_has "status reports 3 of 4 disks online" "Disks online: 3 of 4" "$out"
check_has "status marks the removed disk MISSING" "MISSING" "$out"
check_line "block on a surviving disk is still readable" "  on disk one" "$out"
check_has "block on the missing disk gives a read error" "Error: read failed" "$out"
check_line "shell is still alive after the failed read" "ALIVE-OK" "$out"

echo
echo "--- Damaged disk header ---"
run_sf "fileguard init" > /dev/null
dd if=/dev/zero of="$DATA/disk3.img" bs=1 count=16 conv=notrunc 2>/dev/null
out=$(run_sf "fileguard status")
check_has "damaged header is reported CORRUPT" "CORRUPT" "$out"
check_has "status counts 3 of 4 disks online" "Disks online: 3 of 4" "$out"

echo
echo "=============================================================="
echo " Passed: $PASS    Failed: $FAIL"
echo "=============================================================="

if [ "$FAIL" -eq 0 ]; then
    exit 0
fi
exit 1
