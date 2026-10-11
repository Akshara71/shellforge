#!/bin/bash
# tests/run_lexer_tests.sh - word-length limit tests (Shellforge lexer + FileGuard)
#
# Usage (from the project folder):  ./tests/run_lexer_tests.sh
# Runs a temporary COPY of ./shellforge, so the real data/ folder is untouched.

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

PASS=0
FAIL=0

run_sf() {
    {
        echo "debug off"
        for c in "$@"; do
            echo "$c"
        done
        echo "exit"
    } | timeout 20 "$SF" 2>&1
}

ok()  { PASS=$((PASS + 1)); echo "PASS: $1"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; echo "      expected: $2"; echo "      got:      $3"; }

check_eq() {
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "$2" "$3"; fi
}

check_has() {
    if printf '%s\n' "$3" | grep -qF -- "$2"; then
        ok "$1"
    else
        bad "$1" "$2" "$(printf '%s\n' "$3" | tail -3 | cut -c1-80 | tr '\n' '|')"
    fi
}

check_line() {
    if printf '%s\n' "$3" | grep -qxF -- "$2"; then
        ok "$1"
    else
        bad "$1" "$2" "$(printf '%s\n' "$3" | tail -3 | cut -c1-80 | tr '\n' '|')"
    fi
}

# mk N : print N letter x
mk() { head -c "$1" /dev/zero | tr '\0' x; }

echo "=============================================================="
echo " Shellforge word-length limit tests"
echo " Date : $(date)"
echo "=============================================================="

echo
echo "--- Shell word limit (1023 characters) ---"
out=$(run_sf "echo $(mk 1023)")
len=$(printf '%s\n' "$out" | grep -E '^x+$' | awk '{print length($0)}')
check_eq "a 1023-character word is passed through unchanged" "1023" "$len"

out=$(run_sf "echo $(mk 1024)" "echo ALIVE-OK")
check_has "a 1024-character word is rejected with an error" \
    "Lexer Error : Word too long (maximum 1023 characters)" "$out"
check_eq "the rejected command printed nothing (no cut-off text)" "0" \
    "$(printf '%s\n' "$out" | grep -cE '^x+$')"
check_line "shell is still alive after the rejected line" "ALIVE-OK" "$out"

echo
echo "--- FileGuard text through the shell ---"
out=$(run_sf "fileguard init" "fileguard write 31 \"$(mk 511)\"" "fileguard read 31")
check_has "511 characters are accepted" "Wrote 511 bytes to logical block 31" "$out"
len=$(printf '%s\n' "$out" | grep -E '^  x+$' | awk '{print length($0)}')
check_eq "block 31 reads back all 511 characters (2 spaces + 511 = 513)" "513" "$len"

out=$(run_sf "fileguard init" "fileguard write 32 \"$(mk 1100)\"" "fileguard read 32")
check_has "an over-long text is rejected by the lexer" "Word too long" "$out"
check_has "the rejected write stored nothing (block still empty)" "(empty block)" "$out"

echo
echo "=============================================================="
echo " Passed: $PASS    Failed: $FAIL"
echo "=============================================================="

if [ "$FAIL" -eq 0 ]; then
    exit 0
fi
exit 1
