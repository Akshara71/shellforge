#!/bin/bash
# tests/run_raid5_tests.sh - repeatable tests for FileGuard RAID-5 (rotating parity)
#
# Usage (from the project folder):  ./tests/run_raid5_tests.sh
#
# The tests run a temporary COPY of ./shellforge, so the real data/ folder
# of the project is never touched. FileGuard keeps its disk images in a
# "data" folder next to the program, so the copy gets its own fresh folder.
#
# Expected layout (stripe = block / 3, parity disk = 4 - (stripe % 4)):
#   stripe 0: D1=b0  D2=b1  D3=b2  D4=P
#   stripe 1: D1=b3  D2=b4  D3=P   D4=b5
#   stripe 2: D1=b6  D2=P   D3=b7  D4=b8
#   stripe 3: D1=P   D2=b9  D3=b10 D4=b11
# The tests below use this table, written out by hand, as the reference.

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

# Reference table for logical blocks 0..11 (disk numbers are 1-based)
REF_DATA=(1 2 3 1 2 4 1 3 4 2 3 4)
REF_STRIPE=(0 0 0 1 1 1 2 2 2 3 3 3)
REF_PARITY=(4 4 4 3 3 3 2 2 2 1 1 1)

# Run Shellforge with the given commands, one per argument.
run_sf() {
    {
        echo "debug off"
        for c in "$@"; do
            echo "$c"
        done
        echo "exit"
    } | timeout 30 "$SF" 2>&1
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

# check_all NAME PREFIX "BLOCKS" OUTPUT : for every block B the output has
# the line "  PREFIXB" (that block read back correctly)
check_all() {
    local missing="" b
    for b in $3; do
        printf '%s\n' "$4" | grep -qxF -- "  $2$b" || missing="$missing $b"
    done
    if [ -z "$missing" ]; then
        ok "$1"
    else
        bad "$1" "a line '  $2<block>' for every block in: $3" "missing blocks:$missing"
    fi
}

# stripe_xor_zero STRIPE : prints 0 if the 4 blocks of the stripe (3 data +
# parity) XOR to zero in all 512 bytes, which is exactly what correct
# parity means. Reads the disk image files directly.
stripe_xor_zero() {
    local off=$(( (1 + $1) * 512 )) i bad=0
    local -a A B C D
    read -ra A <<< "$(od -An -v -tu1 -w512 -j "$off" -N 512 "$DATA/disk1.img")"
    read -ra B <<< "$(od -An -v -tu1 -w512 -j "$off" -N 512 "$DATA/disk2.img")"
    read -ra C <<< "$(od -An -v -tu1 -w512 -j "$off" -N 512 "$DATA/disk3.img")"
    read -ra D <<< "$(od -An -v -tu1 -w512 -j "$off" -N 512 "$DATA/disk4.img")"
    if [ "${#A[@]}" -ne 512 ] || [ "${#B[@]}" -ne 512 ] || \
       [ "${#C[@]}" -ne 512 ] || [ "${#D[@]}" -ne 512 ]; then
        echo 1
        return
    fi
    for ((i = 0; i < 512; i++)); do
        if (( (A[i] ^ B[i] ^ C[i] ^ D[i]) != 0 )); then
            bad=1
            break
        fi
    done
    echo $bad
}

# bad_stripes "LIST" : prints the stripes in the list whose parity is wrong
bad_stripes() {
    local s list=""
    for s in $1; do
        [ "$(stripe_xor_zero "$s")" = "0" ] || list="$list $s"
    done
    echo "$list"
}

# xor3 OFFSET COUNT FILE1 FILE2 FILE3 : hex bytes of the XOR of the three
# files at that offset (what rebuilding a lost block would produce)
xor3() {
    local off=$1 n=$2 f1=$3 f2=$4 f3=$5 i out=""
    local -a A B C
    read -ra A <<< "$(od -An -v -tu1 -w"$n" -j "$off" -N "$n" "$f1")"
    read -ra B <<< "$(od -An -v -tu1 -w"$n" -j "$off" -N "$n" "$f2")"
    read -ra C <<< "$(od -An -v -tu1 -w"$n" -j "$off" -N "$n" "$f3")"
    for ((i = 0; i < n; i++)); do
        out="$out $(printf '%02x' $(( A[i] ^ B[i] ^ C[i] )))"
    done
    echo $out
}

echo "=============================================================="
echo " FileGuard RAID-5 (rotating parity) test run"
echo " Date    : $(date)"
echo " Kernel  : $(uname -r)"
echo " GCC     : $(gcc --version | head -1)"
echo " Binary  : $BIN"
echo "=============================================================="

echo
echo "--- init raid5: headers and capacity ---"
out=$(run_sf "fileguard init raid5" "fileguard status")
check_has "init raid5 announces RAID-5 (rotating parity)" "as RAID-5 (rotating parity)" "$out"
check_has "init completes" "Done. Run 'fileguard status'" "$out"
check_has "status shows the RAID-5 volume with 3072 blocks" \
    "Volume: RAID-5 rotating parity over 4 disks, 3072 logical blocks (1572864 bytes)" "$out"
check_has "array state is HEALTHY" "Array state: HEALTHY (4 of 4 disks available)" "$out"
check_has "status reports 4 of 4 disks online" "Disks online: 4 of 4" "$out"
for i in 1 2 3 4; do
    check_eq "disk$i.img is 524800 bytes" "524800" \
        "$(stat -c %s "$DATA/disk$i.img" 2>/dev/null)"
    check_eq "disk$i.img header: version 2 and RAID level 5" \
        "02 00 00 00 05 00 00 00" \
        "$({ od -An -tx1 -j4 -N4 "$DATA/disk$i.img"; od -An -tx1 -j24 -N4 "$DATA/disk$i.img"; } | xargs)"
done

echo
echo "--- Block mapping and rotating parity (checked against the table) ---"
cmds=()
for b in 0 1 2 3 4 5 6 7 8 9 10 11 3071; do
    cmds+=("fileguard layout $b")
done
out=$(run_sf "${cmds[@]}")
for b in 0 1 2 3 4 5 6 7 8 9 10 11; do
    row=$(printf '%13u   Disk %d   %10u   Disk %d' "$b" "${REF_DATA[$b]}" "${REF_STRIPE[$b]}" "${REF_PARITY[$b]}")
    check_line "block $b: Disk ${REF_DATA[$b]}, stripe ${REF_STRIPE[$b]}, parity on Disk ${REF_PARITY[$b]}" "$row" "$out"
done
# 3071 = stripe 1023, third data block; parity disk = 4 - (1023 % 4) = Disk 1
row=$(printf '%13u   Disk %d   %10u   Disk %d' 3071 4 1023 1)
check_line "block 3071 (last): Disk 4, stripe 1023, parity on Disk 1" "$row" "$out"

echo
echo "--- Write and read back ---"
out=$(run_sf "fileguard write 0 \"r5-0\"")
check_has "write reports the data disk and the parity disk" \
    "Wrote 4 bytes to logical block 0 -> Disk 1, block 0 (parity on Disk 4)" "$out"
BLOCKS="0 1 2 3 4 5 6 7 8 9 10 11 100 3071"
cmds=()
for b in $BLOCKS; do
    cmds+=("fileguard write $b \"r5-$b\"")
done
for b in $BLOCKS; do
    cmds+=("fileguard read $b")
done
out=$(run_sf "${cmds[@]}")
check_all "all 14 test blocks read back exactly what was written" "r5-" "$BLOCKS" "$out"

echo
echo "--- Capacity is 3072 blocks ---"
out=$(run_sf "fileguard write 3072 \"x\"" "fileguard read 3072" \
             "fileguard layout 3072" "fileguard read 3071")
check_count "3 out-of-range accesses of block 3072 are rejected" \
    "Error: block must be a number from 0 to 3071" 3 "$out"
check_line "block 3071 is still readable" "  r5-3071" "$out"

echo
echo "--- Parity bytes on the disk images ---"
run_sf "fileguard init raid5" "fileguard write 0 \"A\"" > /dev/null
check_eq "block 0 'A' is on Disk 1, stripe 0 (offset 512)" "41 00" \
    "$(od -An -tx1 -j 512 -N 2 "$DATA/disk1.img" | xargs)"
check_eq "stripe 0 parity (Disk 4) equals block 0 so far" "41 00" \
    "$(od -An -tx1 -j 512 -N 2 "$DATA/disk4.img" | xargs)"
run_sf "fileguard write 1 \"B\"" > /dev/null
check_eq "stripe 0 parity is now 'A' XOR 'B' = 03" "03 00" \
    "$(od -An -tx1 -j 512 -N 2 "$DATA/disk4.img" | xargs)"
run_sf "fileguard write 4 \"AAAA\"" > /dev/null
check_eq "block 4 'AAAA' is on Disk 2, stripe 1 (offset 1024)" "41 41 41 41 00" \
    "$(od -An -tx1 -j 1024 -N 5 "$DATA/disk2.img" | xargs)"
check_eq "stripe 1 parity (Disk 3) equals block 4 so far" "41 41 41 41 00" \
    "$(od -An -tx1 -j 1024 -N 5 "$DATA/disk3.img" | xargs)"
run_sf "fileguard write 3 \"ZZZZ\"" > /dev/null
check_eq "stripe 1 parity is now 'AAAA' XOR 'ZZZZ' = 1b" "1b 1b 1b 1b 00" \
    "$(od -An -tx1 -j 1024 -N 5 "$DATA/disk3.img" | xargs)"

echo
echo "--- Parity stays correct after many writes ---"
run_sf "fileguard init raid5" > /dev/null
cmds=()
for b in $(seq 0 47); do
    cmds+=("fileguard write $b \"parity-test-$b\"")
done
run_sf "${cmds[@]}" > /dev/null
check_eq "stripes 0-15 all have correct parity after 48 writes" "" \
    "$(bad_stripes "$(seq 0 15)")"
check_eq "an untouched stripe (100) is consistent (all zero)" "" \
    "$(bad_stripes "100")"
cmds=()
for b in 0 5 7 20 47; do
    cmds+=("fileguard write $b \"short\"")
done
run_sf "${cmds[@]}" > /dev/null
check_eq "parity is still correct after overwriting with shorter text" "" \
    "$(bad_stripes "$(seq 0 15)")"

echo
echo "--- One disk missing: every block is still readable and writable ---"
for f in 1 2 3 4; do
    run_sf "fileguard init raid5" > /dev/null
    cmds=()
    for b in 0 1 2 3 4 5 6 7 8 9 10 11; do
        cmds+=("fileguard write $b \"blk-$b\"")
    done
    run_sf "${cmds[@]}" > /dev/null
    rm -f "$DATA/disk$f.img"

    # Reads: blocks on the missing disk are rebuilt from the other three
    cmds=("fileguard status")
    for b in 0 1 2 3 4 5 6 7 8 9 10 11; do
        cmds+=("fileguard read $b")
    done
    out=$(run_sf "${cmds[@]}")
    check_has "Disk $f missing: array state is DEGRADED" \
        "Array state: DEGRADED (3 of 4 disks available" "$out"
    check_all "Disk $f missing: all 12 blocks read back correctly" "blk-" \
        "0 1 2 3 4 5 6 7 8 9 10 11" "$out"
    hb=""
    for b in 0 1 2 3 4 5 6 7 8 9 10 11; do
        if [ "${REF_DATA[$b]}" = "$f" ]; then hb=$b; break; fi
    done
    check_has "Disk $f missing: block $hb says it was rebuilt from parity" \
        "(DEGRADED: Disk $f unavailable, block rebuilt from parity)" "$out"

    # Writes while degraded: three different paths (data disk down,
    # parity disk down, some other disk down)
    cmds=()
    for b in 0 1 2 3 4 5 6 7 8 9 10 11; do
        cmds+=("fileguard write $b \"new-$b\"")
    done
    for b in 0 1 2 3 4 5 6 7 8 9 10 11; do
        cmds+=("fileguard read $b")
    done
    cmds+=("echo ALIVE-OK")
    out=$(run_sf "${cmds[@]}")
    check_all "Disk $f missing: 12 blocks written while degraded read back correctly" "new-" \
        "0 1 2 3 4 5 6 7 8 9 10 11" "$out"
    msg=$(printf 'Wrote 5 bytes to logical block %d -> Disk %d unavailable, block %d kept in parity on Disk %d' \
        "$hb" "$f" "${REF_STRIPE[$hb]}" "${REF_PARITY[$hb]}")
    check_has "Disk $f missing: write to its block is kept in the parity" "$msg" "$out"
    pb=""
    for b in 0 1 2 3 4 5 6 7 8 9; do
        if [ "${REF_PARITY[$b]}" = "$f" ]; then pb=$b; break; fi
    done
    msg=$(printf 'Wrote 5 bytes to logical block %d -> Disk %d, block %d (parity not updated)' \
        "$pb" "${REF_DATA[$pb]}" "${REF_STRIPE[$pb]}")
    check_has "Disk $f missing: write to a stripe whose parity was on it says so" "$msg" "$out"
    check_has "Disk $f missing: writes carry the DEGRADED note" \
        "(DEGRADED: Disk $f unavailable)" "$out"
    check_line "Disk $f missing: shell is still alive" "ALIVE-OK" "$out"
done

echo
echo "--- Independent check of the rebuild math (Disk 2 missing) ---"
run_sf "fileguard init raid5" "fileguard write 1 \"lost-data\"" "fileguard write 0 \"keep0\"" \
       "fileguard write 2 \"keep2\"" > /dev/null
# block 1 is on Disk 2, stripe 0 (offset 512). XOR of Disk 1, Disk 3, Disk 4
# at that offset must give back "lost-data" + end-of-string.
check_eq "XOR of the surviving disks equals the data on the lost Disk 2" \
    "6c 6f 73 74 2d 64 61 74 61 00" \
    "$(xor3 512 10 "$DATA/disk1.img" "$DATA/disk3.img" "$DATA/disk4.img")"

echo
echo "--- Two disks lost (RAID-5 cannot rebuild this) ---"
run_sf "fileguard init raid5" "fileguard write 0 \"doomed0\"" \
       "fileguard write 2 \"survivor2\"" > /dev/null
rm -f "$DATA/disk1.img" "$DATA/disk2.img"
out=$(run_sf "fileguard status" "fileguard read 2" "fileguard read 0" \
             "fileguard write 2 \"x\"" "echo ALIVE-OK")
check_has "array state is FAILED" "Array state: FAILED (2 of 4 disks available" "$out"
check_line "a block on a surviving disk can still be read directly" "  survivor2" "$out"
check_has "a block on a lost disk cannot be rebuilt, and says so" "Error: read failed" "$out"
check_has "writes are refused with two disks lost" "Error: write failed" "$out"
check_line "shell is still alive" "ALIVE-OK" "$out"

echo
echo "--- Disk 1 header damaged (CORRUPT counts as failed) ---"
run_sf "fileguard init raid5" "fileguard write 0 \"mine\"" > /dev/null
dd if=/dev/zero of="$DATA/disk1.img" bs=1 count=16 conv=notrunc 2>/dev/null
out=$(run_sf "fileguard status" "fileguard read 0")
check_has "damaged header is reported CORRUPT" "CORRUPT" "$out"
check_has "array state is DEGRADED" "Array state: DEGRADED (3 of 4 disks available" "$out"
check_line "the block on the damaged disk is rebuilt from parity" "  mine" "$out"
check_has "read says it was rebuilt" "block rebuilt from parity" "$out"

echo
echo "--- Persistence and switching RAID levels ---"
run_sf "fileguard init raid5" "fileguard write 42 \"persist me\"" > /dev/null
out=$(run_sf "fileguard read 42" "fileguard status")
check_line "data written in one session is read in a new session" "  persist me" "$out"
check_has "the RAID level is remembered in a new session" \
    "Volume: RAID-5 rotating parity over 4 disks" "$out"
out=$(run_sf "fileguard init raid9" "fileguard init raid5 extra" \
             "fileguard read 42" "echo ALIVE-OK")
check_has "unknown RAID level is rejected" \
    "Error: unknown RAID level 'raid9' (use raid0, raid1 or raid5)" "$out"
check_has "extra init argument shows usage" "Usage: fileguard init [raid0|raid1|raid5]" "$out"
check_line "rejected init commands did not erase the data" "  persist me" "$out"
check_line "shell is still alive" "ALIVE-OK" "$out"
out=$(run_sf "fileguard init raid1" "fileguard read 3071")
check_has "back in RAID-1 block 3071 is out of range" \
    "Error: block must be a number from 0 to 1023" "$out"
out=$(run_sf "fileguard init" "fileguard write 4095 \"raid0-last\"" "fileguard read 4095")
check_line "plain 'init' still creates RAID-0 with 4096 blocks" "  raid0-last" "$out"

echo
echo "=============================================================="
echo " Passed: $PASS    Failed: $FAIL"
echo "=============================================================="

if [ "$FAIL" -eq 0 ]; then
    exit 0
fi
exit 1
