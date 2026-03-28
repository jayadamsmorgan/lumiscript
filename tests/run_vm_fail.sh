#!/bin/sh
set -eu

srcdir="$1"
builddir="$2"
input="$3"
output="$4"
flag="$5"
value="$6"
expected="$7"
stderr_file="$builddir/$(basename "$output").stderr"

"$builddir/lumic" "$srcdir/$input" -o "$builddir/$output"
if "$builddir/lumivm" "$flag" "$value" "$builddir/$output" </dev/null 2>"$stderr_file"; then
    echo "expected VM run to fail" >&2
    exit 1
fi

grep -F "$expected" "$stderr_file" >/dev/null
