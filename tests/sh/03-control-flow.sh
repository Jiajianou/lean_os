# if / while / until / for / case, and break and continue.
if true; then echo if-then; else echo if-else; fi
if false; then echo bad; elif true; then echo elif; else echo else; fi
if false; then echo bad; elif false; then echo bad2; else echo else-taken; fi
i=0
while test $i -lt 3; do
  echo "while $i"
  i=$(echo $i | tr 012 123)
done
n=0
until test $n -ge 2; do
  echo "until $n"
  n=$(echo $n | tr 01 12)
done
for f in alpha beta gamma; do echo "for $f"; done
for a in 1 2 3 4 5; do
  case $a in
    2) continue ;;
    4) break ;;
  esac
  echo "flow $a"
done
x=foo.c
case $x in
  *.h) echo header ;;
  *.c) echo "c file" ;;
  *)   echo other ;;
esac
for v in cat dog bird; do
  case $v in cat|dog) echo "$v pet" ;; *) echo "$v wild" ;; esac
done
case abc in a*) echo starts-a ;; esac
nested() {
  for outer in 1 2; do
    for inner in a b; do
      if test $inner = b; then continue; fi
      echo "nest $outer$inner"
    done
  done
}
nested
