#!/bin/sh
set -eu

srcdir="$1"
builddir="$2"
input="$3"
expected="$4"
stderr_file="$builddir/$(basename "$input").stderr"

if "$builddir/lumic" "$srcdir/$input" -o "$builddir/should-not-exist.lbc" 2>"$stderr_file"; then
    echo "expected compilation to fail for $input" >&2
    exit 1
fi

if ! grep -F "$expected" "$stderr_file" >/dev/null; then
    echo "expected error message not found for $input" >&2
    cat "$stderr_file" >&2
    exit 1
fi
