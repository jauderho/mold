#!/bin/bash
export LC_ALL=C
set -e
CC="${CC:-cc}"
CXX="${CXX:-c++}"
GCC="${GCC:-gcc}"
GXX="${GXX:-g++}"
OBJDUMP="${OBJDUMP:-objdump}"
MACHINE="${MACHINE:-$(uname -m)}"
testname=$(basename "$0" .sh)
echo -n "Testing $testname ... "
cd "$(dirname "$0")"/../..
mold="$(pwd)/mold"
t=out/test/elf/$testname
mkdir -p $t

cat <<EOF | $CXX -c -o $t/a.o -xc++ -fPIC -
int main() {
  try {
    throw 0;
  } catch (int x) {
    return x;
  }
  return 1;
}
EOF

$CXX -B. -o $t/exe $t/a.o -static
$QEMU $t/exe

$CXX -B. -o $t/exe $t/a.o
$QEMU $t/exe

$CXX -B. -o $t/exe $t/a.o -Wl,--gc-sections
$QEMU $t/exe

$CXX -B. -o $t/exe $t/a.o -static -Wl,--gc-sections
$QEMU $t/exe

if [ $MACHINE = x86_64 ]; then
  $CXX -B. -o $t/exe $t/a.o -mcmodel=large
  $t/exe

  $CXX -B. -o $t/exe $t/a.o -static -mcmodel=large
  $t/exe
elif [ $MACHINE = aarch64 ]; then
  # The -mcmodel=large option is incompatible with -fPIC on aarch64, see
  # https://$GCC.gnu.org/onlinedocs/$GCC/AArch64-Options.html#index-mcmodel_003dlarge
  $CXX -B. -o $t/exe $t/a.o -mcmodel=large -fno-PIC
  $t/exe

  $CXX -B. -o $t/exe $t/a.o -static -mcmodel=large -fno-PIC
  $t/exe
fi

echo OK
