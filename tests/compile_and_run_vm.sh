#!/bin/sh
set -eu

srcdir="$1"
builddir="$2"
input="$3"
output="$4"
tick_input="$5"
expected="$6"
actual="$builddir/$(basename "$output").out"

"$builddir/lumic" "$srcdir/$input" -o "$builddir/$output"
"$builddir/lumivm" "$builddir/$output" <"$srcdir/$tick_input" >"$actual"
diff -u "$srcdir/$expected" "$actual"
