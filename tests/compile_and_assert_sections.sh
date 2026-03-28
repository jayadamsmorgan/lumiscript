#!/bin/sh
set -eu

srcdir="$1"
builddir="$2"
input="$3"
output="$4"
program_type="$5"
constant_count="$6"
global_count="$7"
key_count="$8"
max_stack="$9"
constants="${10}"
globals="${11}"
keys="${12}"
init_code="${13}"
update_code="${14}"
render_code="${15}"

"$builddir/lumic" "$srcdir/$input" -o "$builddir/$output"
"$builddir/bytecode_assert_sections" "$builddir/$output" "$program_type" "$constant_count" "$global_count" "$key_count" "$max_stack" "$constants" "$globals" "$keys" "$init_code" "$update_code" "$render_code"
