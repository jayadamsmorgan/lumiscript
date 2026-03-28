#!/bin/sh
set -eu

srcdir="$1"
builddir="$2"
input="$3"
output="$4"

"$builddir/lumic" "$srcdir/$input" -o "$builddir/$output"
test -s "$builddir/$output"
