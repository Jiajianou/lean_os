# Field splitting, and globbing.
list="one two three"
for w in $list; do echo "split $w"; done
echo "quoted [$list]"
set -- a "b c" d
echo "count=$#"
for w in "$@"; do echo "at [$w]"; done
for w in $*; do echo "star [$w]"; done
echo "joined [$*]"
shift
echo "after-shift count=$# first=[$1]"
mkdir -p globdir
echo 1 > globdir/one.txt
echo 2 > globdir/two.txt
echo 3 > globdir/three.log
for f in globdir/*.txt; do echo "glob $f"; done
for f in globdir/*.none; do echo "nomatch $f"; done
for f in globdir/?ne.txt; do echo "question $f"; done
cd globdir
for f in *.txt; do echo "local $f"; done
cd ..
