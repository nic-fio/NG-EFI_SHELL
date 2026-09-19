#!/bin/sh
# Runs every tests/host/*.nsb with the host build and compares the output
# (stdout + stderr) with the matching .out file. UPDATE=1 rewrites the .out files.
NESH=$(realpath "$1")
DIR=$(cd "$(dirname "$0")/host" && pwd)
pass=0; fail=0
for t in "$DIR"/*.nsb; do
    name=$(basename "$t" .nsb)
    work=$(mktemp -d)
    cp "$t" "$work/test.nsb"
    actual=$(cd "$work" && NESH_FS=. "$NESH" test.nsb arg1 "arg two" 2>&1; echo "[exit $?]")
    rm -rf "$work"
    if [ "$UPDATE" = 1 ]; then
        printf '%s\n' "$actual" > "$DIR/$name.out"
    fi
    if [ "$actual" = "$(cat "$DIR/$name.out" 2>/dev/null)" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $name"
        printf '%s\n' "$actual" | diff "$DIR/$name.out" - | head -20
    fi
done
echo "host tests: $pass passed, $fail failed"
[ $fail = 0 ]
