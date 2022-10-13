#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -o $t/a.o -c -xc -
void foo() {}
EOF

./mold -shared -o $t/b.so $t/a.o

readelf -WS $t/b.so | grep -Fq ' .hash'
readelf -WS $t/b.so | grep -Fq ' .gnu.hash'
