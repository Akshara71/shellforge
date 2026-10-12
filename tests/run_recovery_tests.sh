#!/bin/bash
# tests/run_recovery_tests.sh - tests for fail, rebuild and verify (RAID-1 and RAID-5)
#
# Usage (from the project folder):  ./tests/run_recovery_tests.sh
#
# The tests run a temporary COPY of ./shellforge, so the real data/ folder
# of the project is never touched. FileGuard keeps its disk images in a
# "data" folder next to the program, so the copy gets its own fresh folder.
#
# Strongest check used below: save a copy of a disk image, fail that disk,
# rebuild it, then compare the rebuilt image with the saved copy using cmp.
# Identical means every byte, header included, was restored correctly.

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
SAVED="$WORK/saved.img"

PASS=0
FAIL=0
ALL24="0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23"

# Run Shellforge with the given commands, one per argument.
run_sf() {
    {
        echo "debug off"
        for c in "$@"; do
            echo "$c"
        done
        echo "exit"
    } | timeout 60 "$SF" 2>&1
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

check_has() {
    if printf '%s\n' "$3" | grep -qF -- "$2"; then
        ok "$1"
    else
        bad "$1" "$2" "$(printf '%s\n' "$3" | tail -4 | cut -c1-100 | tr '\n' '|')"
    fi
}

check_line() {
    if printf '%s\n' "$3" | grep -qxF -- "$2"; then
        ok "$1"
    else
        bad "$1" "$2" "$(printf '%s\n' "$3" | tail -4 | cut -c1-100 | tr '\n' '|')"
    fi
}

check_count() {
    local n
    n=$(printf '%s\n' "$4" | grep -cF -- "$2")
    if [ "$n" = "$3" ]; then
        ok "$1"
    else
        bad "$1" "$3 lines containing: $2" "$n lines"
    fi
}

check_eq() {
    if [ "$2" = "$3" ]; then
        ok "$1"
    else
        bad "$1" "$2" "$3"
    fi
}

# check_all NAME PREFIX "BLOCKS" OUTPUT : every block B has the line "  PREFIXB"
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

# write_cmds PREFIX "BLOCKS" : print one write command per block
write_cmds() {
    local b
    for b in $2; do
        echo "fileguard write $b \"$1$b\""
    done
}

# read_cmds "BLOCKS" : print one read command per block
read_cmds() {
    local b
    for b in $1; do
        echo "fileguard read $b"
    done
}

# run_list : run_sf with the commands read from stdin (one per line)
run_list() {
    local -a cmds=()
    local line
    while IFS= read -r line; do
        cmds+=("$line")
    done
    run_sf "${cmds[@]}"
}

# stripe_xor_zero STRIPE : 0 if the 4 blocks of a RAID-5 stripe XOR to zero
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

bad_stripes() {
    local s list=""
    for s in $1; do
        [ "$(stripe_xor_zero "$s")" = "0" ] || list="$list $s"
    done
    echo "$list"
}

echo "=============================================================="
echo " FileGuard failure, rebuild and verify test run"
echo " Date    : $(date)"
echo " Kernel  : $(uname -r)"
echo " GCC     : $(gcc --version | head -1)"
echo " Binary  : $BIN"
echo "=============================================================="

echo
echo "--- fail command ---"
out=$(run_sf "fileguard fail 1")
check_has "fail before init is refused" "Error: the volume is not initialised" "$out"
run_sf "fileguard init raid5" "fileguard write 0 \"alpha\"" > /dev/null
out=$(run_sf "fileguard fail 2" "fileguard status" "fileguard fail 2" \
             "fileguard fail 5" "fileguard fail 0" "fileguard fail abc" \
             "fileguard fail" "echo ALIVE-OK")
check_has "fail reports the simulated failure" "Simulating failure of Disk 2" "$out"
[ -e "$DATA/disk2.img" ]
check_eq "the image file of Disk 2 is really gone (0 = gone)" "1" "$?"
check_has "status shows the array DEGRADED" "Array state: DEGRADED (3 of 4 disks available" "$out"
check_has "failing the same disk twice is refused" "Error: Disk 2 is already failed" "$out"
check_count "bad disk numbers (5, 0, abc) are rejected" \
    "Error: disk must be a number from 1 to 4" 3 "$out"
check_has "fail without a disk number shows usage" "Usage: fileguard fail <disk>" "$out"
check_line "shell is still alive" "ALIVE-OK" "$out"
out=$(run_sf "fileguard init raid0" "fileguard fail 1")
check_has "failing a RAID-0 disk warns that data is lost" \
    "Warning: RAID-0 has no redundancy, so the data stored on Disk 1 is lost." "$out"
out=$(run_sf "fileguard init raid1" "fileguard fail 3" "fileguard fail 1" "fileguard fail 2")
check_has "RAID-1: failing an unused disk does not affect the mirror" \
    "Disk 3 is not used by RAID-1, so the mirror is not affected." "$out"
check_has "RAID-1: failing one mirror disk degrades it" \
    "The mirror is now DEGRADED" "$out"
check_has "RAID-1: failing both mirror disks warns" \
    "Warning: both mirror disks have failed" "$out"
out=$(run_sf "fileguard init raid5" "fileguard fail 1" "fileguard fail 2")
check_has "RAID-5: a second failure warns that recovery is impossible" \
    "Warning: 2 disks have failed. RAID-5 can only recover from one failed disk." "$out"

echo
echo "--- RAID-5: rebuild each disk, restoring the exact original image ---"
for f in 1 2 3 4; do
    write_cmds "pre-" "$ALL24" > "$WORK/pre.txt"
    { echo "fileguard init raid5"; cat "$WORK/pre.txt"; } | run_list > /dev/null
    cp "$DATA/disk$f.img" "$SAVED"
    out=$(run_sf "fileguard fail $f" "fileguard rebuild $f" "fileguard status")
    check_has "Disk $f: rebuild reports the RAID-5 volume" \
        "Rebuilt Disk $f of the RAID-5 (rotating parity) volume." "$out"
    check_has "Disk $f: all 1024 blocks were compared and match" \
        "compared every block with the surviving disks: 1024 of 1024 match." "$out"
    check_has "Disk $f: it is ONLINE again" "Disk $f is ONLINE again." "$out"
    check_has "Disk $f: the automatic full verify says CONSISTENT" "Result: CONSISTENT" "$out"
    check_has "Disk $f: array is HEALTHY again" \
        "Array state: HEALTHY (4 of 4 disks available)" "$out"
    cmp -s "$SAVED" "$DATA/disk$f.img"
    check_eq "Disk $f: rebuilt image is byte-for-byte identical to the original" "0" "$?"
    out=$(read_cmds "$ALL24" | run_list)
    check_all "Disk $f: all 24 blocks read back correctly after the rebuild" "pre-" "$ALL24" "$out"
done

echo
echo "--- RAID-5: data written while a disk was down survives the rebuild ---"
for f in 1 2 3 4; do
    { echo "fileguard init raid5"; write_cmds "pre-" "$ALL24"; \
      echo "fileguard fail $f"; write_cmds "post-" "$ALL24"; \
      echo "fileguard rebuild $f"; } | run_list > /dev/null
    out=$(run_sf "fileguard status" "fileguard verify")
    check_has "Disk $f: array is HEALTHY after the rebuild" "Array state: HEALTHY" "$out"
    check_has "Disk $f: verify says CONSISTENT" "Result: CONSISTENT" "$out"
    out=$(read_cmds "$ALL24" | run_list)
    check_all "Disk $f: blocks written while degraded read back correctly" "post-" "$ALL24" "$out"
    check_eq "Disk $f: stripes 0-7 pass the independent parity check in bash" "" \
        "$(bad_stripes "0 1 2 3 4 5 6 7")"
done

echo
echo "--- RAID-5: a damaged (CORRUPT) disk can be rebuilt too ---"
write_cmds "pre-" "$ALL24" > "$WORK/pre.txt"
{ echo "fileguard init raid5"; cat "$WORK/pre.txt"; } | run_list > /dev/null
cp "$DATA/disk3.img" "$SAVED"
dd if=/dev/zero of="$DATA/disk3.img" bs=1 count=16 conv=notrunc 2>/dev/null
out=$(run_sf "fileguard status" "fileguard rebuild 3")
check_has "the damaged disk is shown as CORRUPT before the rebuild" "CORRUPT" "$out"
check_has "the rebuild accepts it and succeeds" "Disk 3 is ONLINE again." "$out"
cmp -s "$SAVED" "$DATA/disk3.img"
check_eq "the rebuilt image is identical to the original" "0" "$?"

echo
echo "--- RAID-1: rebuild a mirror disk ---"
for f in 1 2; do
    { echo "fileguard init raid1"; write_cmds "pre-" "$ALL24"; } | run_list > /dev/null
    cp "$DATA/disk$f.img" "$SAVED"
    out=$(run_sf "fileguard fail $f" "fileguard rebuild $f" "fileguard status")
    check_has "Disk $f: rebuild reports the RAID-1 volume" \
        "Rebuilt Disk $f of the RAID-1 (mirror) volume." "$out"
    check_has "Disk $f: all 1024 blocks match" \
        "compared every block with the surviving disks: 1024 of 1024 match." "$out"
    check_has "Disk $f: the automatic full verify says CONSISTENT" "Result: CONSISTENT" "$out"
    check_has "Disk $f: mirror is HEALTHY again" "Mirror state: HEALTHY" "$out"
    cmp -s "$SAVED" "$DATA/disk$f.img"
    check_eq "Disk $f: rebuilt image is byte-for-byte identical to the original" "0" "$?"

    { echo "fileguard init raid1"; write_cmds "pre-" "$ALL24"; \
      echo "fileguard fail $f"; write_cmds "post-" "$ALL24"; \
      echo "fileguard rebuild $f"; } | run_list > /dev/null
    cmp -s -i 512 "$DATA/disk1.img" "$DATA/disk2.img"
    check_eq "Disk $f: copies are identical after rebuilding with newer data" "0" "$?"
    out=$(read_cmds "$ALL24" | run_list)
    check_all "Disk $f: blocks written while degraded read back correctly" "post-" "$ALL24" "$out"
done
out=$(run_sf "fileguard init raid1" "fileguard fail 3" "fileguard rebuild 3" "fileguard status")
check_has "an unused mirror disk is simply recreated" \
    "Disk 3 is not used by RAID-1: recreated as an empty disk" "$out"
check_has "the mirror is still HEALTHY" "Mirror state: HEALTHY" "$out"
out=$(run_sf "fileguard init raid1" "fileguard fail 1" "fileguard fail 2" "fileguard rebuild 1")
check_has "with both mirror disks gone, rebuild is refused" \
    "Error: cannot rebuild Disk 1: the other mirror disk is also unavailable" "$out"

echo
echo "--- Rebuild is refused when it cannot be done safely ---"
rm -rf "$DATA"          # start from an empty folder: no volume exists yet
out=$(run_sf "fileguard rebuild 1")
check_has "rebuild before init is refused" \
    "Error: cannot rebuild Disk 1: the volume is not initialised" "$out"
run_sf "fileguard init raid5" "fileguard write 0 \"keep\"" > /dev/null
out=$(run_sf "fileguard rebuild 2" "fileguard read 0")
check_has "rebuilding a healthy disk is refused" \
    "Error: cannot rebuild Disk 2: that disk is not failed" "$out"
check_line "and the data is untouched" "  keep" "$out"
out=$(run_sf "fileguard fail 1" "fileguard fail 2" "fileguard rebuild 1" "fileguard status" \
             "echo ALIVE-OK")
check_has "RAID-5 with two failed disks cannot be rebuilt" \
    "Error: cannot rebuild Disk 1: another disk is also unavailable" "$out"
check_has "the refused rebuild left both disks MISSING" "Disks online: 2 of 4" "$out"
check_line "shell is still alive" "ALIVE-OK" "$out"
out=$(run_sf "fileguard init raid0" "fileguard fail 2" "fileguard rebuild 2")
check_has "RAID-0 cannot be rebuilt (no redundancy)" \
    "Error: cannot rebuild Disk 2: RAID-0 has no redundancy" "$out"
out=$(run_sf "fileguard rebuild 9" "fileguard rebuild abc" "fileguard rebuild")
check_count "bad disk numbers are rejected" \
    "Error: disk must be a number from 1 to 4" 2 "$out"
check_has "rebuild without a disk shows usage" "Usage: fileguard rebuild <disk>" "$out"

echo
echo "--- verify: detects problems, does not repair them ---"
rm -rf "$DATA"          # start from an empty folder: no volume exists yet
out=$(run_sf "fileguard verify")
check_has "verify before init is refused" "Error: the volume is not initialised" "$out"
{ echo "fileguard init raid5"; write_cmds "blk-" "0 1 2 3 4 5 6 7 8 9 10 11"; \
  echo "fileguard write 0 \"alpha\""; } | run_list > /dev/null
out=$(run_sf "fileguard verify")
check_has "RAID-5: a healthy volume checks out" \
    "Verify (RAID-5 parity): checked 1024 stripes: 1024 with correct parity, 0 wrong" "$out"
check_has "RAID-5: healthy volume is CONSISTENT" "Result: CONSISTENT" "$out"
# damage the first byte of block 0 (Disk 1, stripe 0, file offset 512)
printf 'X' | dd of="$DATA/disk1.img" bs=1 seek=512 conv=notrunc 2>/dev/null
out=$(run_sf "fileguard read 0" "fileguard verify")
check_line "RAID-5: a normal read does NOT notice the corruption" "  Xlpha" "$out"
check_has "RAID-5: verify detects it" \
    "Verify (RAID-5 parity): checked 1024 stripes: 1023 with correct parity, 1 wrong" "$out"
check_has "RAID-5: verify names the damaged stripe" \
    "Result: INCONSISTENT - first differing stripes: 0" "$out"
check_has "verify says it does not repair" "it does not repair it" "$out"
# damage a parity block (stripe 3: parity is on Disk 1, file offset 2048)
run_sf "fileguard init raid5" "fileguard write 9 \"data9\"" > /dev/null
printf 'Q' | dd of="$DATA/disk1.img" bs=1 seek=2048 conv=notrunc 2>/dev/null
out=$(run_sf "fileguard verify")
check_has "RAID-5: a damaged parity block is detected too (stripe 3)" \
    "first differing stripes: 3" "$out"
out=$(run_sf "fileguard init raid5" "fileguard fail 3" "fileguard verify")
check_has "RAID-5: verify with a failed disk explains what to do" \
    "Error: cannot verify: Disk 3 is unavailable (run 'fileguard rebuild 3')" "$out"
{ echo "fileguard init raid1"; write_cmds "blk-" "0 1 2 3 4 5 6 7 8 9 10 11"; } | run_list > /dev/null
out=$(run_sf "fileguard verify")
check_has "RAID-1: a healthy mirror checks out" \
    "Verify (RAID-1 mirror): compared 1024 blocks of Disk 1 and Disk 2: 1024 identical, 0 different" "$out"
check_has "RAID-1: healthy mirror is CONSISTENT" "Result: CONSISTENT" "$out"
# damage block 3 on Disk 2 only (file offset (1+3)*512 = 2048)
printf 'Z' | dd of="$DATA/disk2.img" bs=1 seek=2048 conv=notrunc 2>/dev/null
out=$(run_sf "fileguard read 3" "fileguard verify")
check_line "RAID-1: reads use Disk 1, so they do not see the damaged copy" "  blk-3" "$out"
check_has "RAID-1: verify detects that the copies differ" \
    "1023 identical, 1 different" "$out"
check_has "RAID-1: verify names the differing block" \
    "Result: INCONSISTENT - first differing blocks: 3" "$out"
out=$(run_sf "fileguard init raid0" "fileguard verify")
check_has "RAID-0: verify says there is nothing to cross-check" \
    "RAID-0 keeps no copies or parity" "$out"
out=$(run_sf "fileguard verify extra" "echo ALIVE-OK")
check_has "verify with extra arguments shows usage" "Usage: fileguard verify" "$out"
check_line "shell is still alive" "ALIVE-OK" "$out"

echo
echo "=============================================================="
echo " Passed: $PASS    Failed: $FAIL"
echo "=============================================================="

if [ "$FAIL" -eq 0 ]; then
    exit 0
fi
exit 1
