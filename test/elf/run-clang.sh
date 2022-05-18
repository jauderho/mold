#!/bin/bash
export LC_ALL=C
set -e
CC="${TEST_CC:-cc}"
CXX="${TEST_CXX:-c++}"
GCC="${TEST_GCC:-gcc}"
GXX="${TEST_GXX:-g++}"
OBJDUMP="${OBJDUMP:-objdump}"
MACHINE="${MACHINE:-$(uname -m)}"
testname=$(basename "$0" .sh)
echo -n "Testing $testname ... "
cd "$(dirname "$0")"/../..
t=out/test/elf/$testname
mkdir -p $t

[ "$CC" = cc ] || { echo skipped; exit; }

# ASAN doesn't work with LD_PRELOAD
nm mold-wrapper.so | grep -Pq '__[at]san_init' && { echo skipped; exit; }

which clang >& /dev/null || { echo skipped; exit; }

cat <<'EOF' | $CC -xc -c -o $t/a.o -
#include <stdio.h>

int main() {
  printf("Hello\n");
  return 0;
}
EOF

LD_PRELOAD=`pwd`/mold-wrapper.so MOLD_PATH=`pwd`/mold \
  clang -o $t/exe $t/a.o -fuse-ld=/usr/bin/ld
readelf -p .comment $t/exe > $t/log
grep -q mold $t/log

echo OK
