#!/bin/sh
BS=/tmp/bs

echo "== m98boot: a build on this machine =="

rm -rf $BS
mkdir -p $BS
cd $BS

echo "== step 0: the assembler, on a directive C++ needs =="
cp /tests/bootstrap/cfi.cc cfi.cc
g++ -O2 -S cfi.cc -o cfi.s
echo "-- what the compiler wrote:"
toybox grep -n "cfi_personality" cfi.s
echo "-- what the assembler makes of it:"
as cfi.s -o cfi.o
echo "-- as exit: $?"

echo "== step 1: a C++ translation unit through cc1plus =="
cp /tests/bootstrap/bigtu.cc bigtu.cc
measure cxx-tu g++ -O2 -c bigtu.cc -o bigtu.o

echo "== step 2: somebody else's source tree =="
toybox cp -r /tests/bzip2 bzip2
cd $BS/bzip2

CF="-Wall -Winline -O2 -D_FILE_OFFSET_BITS=64"

echo "== step 3: one C translation unit =="
measure c-tu gcc $CF -c bzlib.c -o /tmp/bs/c-tu.o

echo "== step 4: bzip2, serially =="
measure bzip2-j1 make -j1 CFLAGS="$CF" libbz2.a bzip2 bzip2recover

echo "== step 5: bzip2's own test suite, on this machine =="
measure bzip2-test make CFLAGS="$CF" test

echo "== step 6: objects, against the cross compiler's =="
for o in blocksort.o huffman.o crctable.o randtable.o compress.o decompress.o bzlib.o bzip2.o; do
    if cmp -s $o /tests/bzip2-ref/$o; then
        echo "identical: $o"
    else
        echo "differs: $o"
    fi
done

echo "== step 7: disk =="
toybox du -sk $BS
toybox du -sk /usr
toybox du -sk /bin
toybox df -k /

echo "== step 8: bzip2 again, four jobs at once =="
make clean
measure -t 900 bzip2-j4 make -j4 CFLAGS="$CF" libbz2.a bzip2 bzip2recover

echo "== m98boot done =="
