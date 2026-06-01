#!/bin/sh
set -eu

srcdir="$1"
builddir="$2"

"$builddir/lumic" -O0 "$srcdir/tests/optimization.lumi" -o "$builddir/optimization-o0.lbc"
"$builddir/lumic" -O1 "$srcdir/tests/optimization.lumi" -o "$builddir/optimization-o1.lbc"
"$builddir/lumic" -O2 "$srcdir/tests/optimization.lumi" -o "$builddir/optimization-o2.lbc"
"$builddir/lumic" -O3 "$srcdir/tests/optimization.lumi" -o "$builddir/optimization-o3.lbc"
"$builddir/lumic" -O2 "$srcdir/tests/optimization-cse.lumi" -o "$builddir/optimization-cse-o2.lbc"
"$builddir/lumic" -O3 "$srcdir/tests/optimization-cse.lumi" -o "$builddir/optimization-cse-o3.lbc"

size_o0=$(wc -c <"$builddir/optimization-o0.lbc")
size_o1=$(wc -c <"$builddir/optimization-o1.lbc")
size_o2=$(wc -c <"$builddir/optimization-o2.lbc")
size_o3=$(wc -c <"$builddir/optimization-o3.lbc")
size_cse_o2=$(wc -c <"$builddir/optimization-cse-o2.lbc")
size_cse_o3=$(wc -c <"$builddir/optimization-cse-o3.lbc")

if [ "$size_o2" -ge "$size_o0" ]; then
    echo "expected -O2 bytecode to be smaller than -O0: O0=$size_o0 O2=$size_o2" >&2
    exit 1
fi
if [ "$size_o2" -ge "$size_o1" ]; then
    echo "expected -O2 bytecode to be smaller than -O1: O1=$size_o1 O2=$size_o2" >&2
    exit 1
fi
if [ "$size_o3" -ge "$size_o2" ]; then
    echo "expected -O3 bytecode to be smaller than -O2: O2=$size_o2 O3=$size_o3" >&2
    exit 1
fi
if [ "$size_cse_o3" -ge "$size_cse_o2" ]; then
    echo "expected CSE -O3 bytecode to be smaller than -O2: O2=$size_cse_o2 O3=$size_cse_o3" >&2
    exit 1
fi

"$builddir/lumivm" "$builddir/optimization-o3.lbc" <"$srcdir/tests/optimization-input.txt" >"$builddir/optimization-o3.out"
diff -u "$srcdir/tests/optimization-expected.txt" "$builddir/optimization-o3.out"
"$builddir/lumivm" "$builddir/optimization-cse-o3.lbc" <"$srcdir/tests/optimization-cse-input.txt" >"$builddir/optimization-cse-o3.out"
diff -u "$srcdir/tests/optimization-cse-expected.txt" "$builddir/optimization-cse-o3.out"
