# M99: `set -e` and the rest of the option family.
#
# `set` here took every argument as a positional parameter, so `set -e`
# replaced the script's arguments with the single word `-e` instead of
# turning on errexit. CPython's Modules/makesetup is `#! /bin/sh` and
# then `set -e` on line 2, so by line 58 its own argument parser was
# looking at `-e` and printing its usage - after configure had run 753
# checks and written pyconfig.h.
#
# The host's shell is the oracle. Nothing below says what the right
# answer is, including the question of exactly which constructs are
# exempt from errexit, which is the part everyone gets wrong.

# ---- options do not touch the positional parameters -------------------
set a b c
echo "before: $# [$1] [$3]"
set -e
echo "after set -e: $# [$1] [$3]"
set +e
echo "after set +e: $# [$1] [$3]"
set -- x y
echo "after set --: $# [$1]"
set --
echo "after bare set --: $#"

# ---- errexit stops on a failure -----------------------------------------
sh -c 'set -e; echo one; false; echo two'; echo "status=$?"

# ---- and does not stop on a status that is being asked about ----------
sh -c '
set -e
if false; then echo taken; fi
if test -f /definitely-not-here; then echo found; fi
false || echo "or-arm ran"
true && echo "and-arm ran"
! false
while false; do echo loop; done
until true; do echo loop; done
echo survived
'

# ---- a failure inside a function still stops the script ---------------
sh -c 'set -e; f() { false; echo "not reached"; }; f; echo "also not reached"'
echo "function status=$?"

# ---- +e turns it back off ---------------------------------------------
sh -c 'set -e; set +e; false; echo survived'

# ---- noglob -----------------------------------------------------------
touch globtest-a.tmp globtest-b.tmp
echo globtest-*.tmp
set -f
echo globtest-*.tmp
set +f
echo globtest-*.tmp
rm -f globtest-a.tmp globtest-b.tmp

# ---- an option this shell does not have is an error, not a no-op ------
sh -c 'set -Z' 2>/dev/null; echo "bad option status=$?"
