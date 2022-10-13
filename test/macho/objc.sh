#!/bin/bash
export LC_ALL=C
set -e
testname=$(basename "$0" .sh)
echo -n "Testing $testname ... "
t=out/test/macho/$(uname -m)/$testname
mkdir -p $t

cat <<EOF | cc -o $t/a.o -c -xobjective-c -
#import <Foundation/NSObject.h>
@interface MyClass : NSObject
@end
@implementation MyClass
@end
EOF

ar rcs $t/b.a $t/a.o

cat <<EOF | cc -o $t/c.o -c -xc -
int main() {}
EOF

cc -o $t/exe $t/c.o $t/b.a
! nm $t/exe | grep -q _OBJC_CLASS_ || false

! cc -o $t/exe $t/c.o $t/b.a -Wl,-ObjC > $t/log 2>&1
grep -q _OBJC_CLASS_ $t/log

echo OK
