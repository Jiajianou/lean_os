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

# ---- M99: what a generated configure script needs from patterns -------
#
# `case` is where a shell script branches, and a generated one brackets
# everything. This matcher understood `*` and `?` and nothing else, so
# every bracket expression fell through to the default arm - silently,
# which is the worst way for a pattern matcher to be wrong.
#
# The one that found it, from CPython's configure, asking whether a
# directory name is absolute:
case "${exec_prefix}/bin" in
  [\\/$]* | ?:[\\/]* ) echo "absolute" ;;
  * ) echo "not absolute" ;;
esac
case "/usr/local" in [\\/$]* ) echo "slash is absolute" ;; * ) echo "no" ;; esac
case "relative/path" in [\\/$]* ) echo "yes" ;; * ) echo "relative" ;; esac
# Ranges, negation, a literal ] first, a literal - last.
for w in a5 z9 Q _ - ']' '['; do
  case $w in
    [a-z][0-9]) echo "$w: lower-digit" ;;
    [A-Z])      echo "$w: upper" ;;
    [!a-zA-Z]*) echo "$w: not a letter" ;;
    *)          echo "$w: other" ;;
  esac
done
case ']' in []abc]) echo "bracket-first is literal" ;; *) echo "no" ;; esac
case '-' in [ab-]) echo "hyphen-last is literal" ;; *) echo "no" ;; esac
# A backslash quotes the next character, so this is a real star.
case '*' in \*) echo "escaped star" ;; *) echo "no" ;; esac
# An unmatched [ is a literal [ and not a pattern.
case '[' in [) echo "literal bracket" ;; *) echo "no" ;; esac

# ---- M99: "$@" and the fields it does and does not make ---------------
#
# `"$@"` with no positional parameters is ZERO fields. It was one empty
# field here, so `for x in "$@"` with no arguments ran its body once.
set --
for x in "$@"; do echo "ran with [$x]"; done
echo "loop over empty \$@ done"
# But adjacent text still makes one field.
echo a"$@"b
# `"$*"` is different and is one field even when empty.
for x in "$*"; do echo "star gives [$x]"; done

# `${1+"$@"}` is how every autoconf script forwards its arguments, and
# it has to keep the fields apart.
set -- one "two three" four
for x in ${1+"$@"}; do echo "<$x>"; done
echo "count=$#"
# The quoted-word bug beside it: the quotes inside the ${} word were
# literal characters, so the expansion inside them never happened.
y="a b"
echo "[${y+"$y"}]"
echo "[${unset_one+"$y"}]"
echo "[${unset_one-"$y"}]"
