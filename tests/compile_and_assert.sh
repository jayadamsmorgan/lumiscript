#!/bin/sh
set -eu

srcdir="$1"
builddir="$2"
input="$3"
output="$4"
program_type="$5"
constant_count="$6"
global_count="$7"
max_stack="$8"
constants="$9"
globals="${10}"
code="${11}"

"$builddir/lumic" "$srcdir/$input" -o "$builddir/$output"
"$builddir/bytecode_assert" "$builddir/$output" "$program_type" "$constant_count" "$global_count" "$max_stack" "$constants" "$globals" "$code"
