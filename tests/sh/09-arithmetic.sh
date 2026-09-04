# M99: $(( )), because somebody else's configure asks for it by name.
#
# CPython's configure decides whether the shell running it is fit for the
# job by evaluating a block it calls `as_required`, whose last line is
# `test $(( 1 + 1 )) = 2 || exit 1`. A shell without this is rejected
# before configure prints anything.
#
# Nothing below says what the right answer is. POSIX says arithmetic
# expansion means what the C operators mean, so the host's own shell is
# the oracle and the C standard is behind it.
echo $(( 1 + 1 ))
echo $((2*3+4)) $((2*(3+4)))
echo $(( 7 / 2 )) $(( 7 % 2 )) $(( -7 / 2 ))
echo $(( 1 << 10 )) $(( 4096 >> 3 ))
echo $(( 1 < 2 )) $(( 2 < 1 )) $(( 2 <= 2 )) $(( 3 > 4 )) $(( 4 >= 4 ))
echo $(( 5 == 5 )) $(( 5 != 5 ))
echo $(( 12 & 10 )) $(( 12 | 10 )) $(( 12 ^ 10 ))
echo $(( 0 || 3 )) $(( 1 && 0 )) $(( !0 )) $(( !5 ))
echo $(( ~0 )) $(( -(-3) )) $(( +4 ))
# Variables, named without a dollar, which is the form that makes a loop
# counter readable.
i=5
echo $(( i + 1 )) $(( i * i ))
echo $(( $i + 1 ))
# An unset name is zero. POSIX says so, and it is why `$(( n + 1 ))`
# works on the first pass before n has ever been assigned.
echo [$(( unsetname + 1 ))]
empty=
echo [$(( empty + 7 ))]
# Bases: 0x is hex and a leading zero is octal, exactly as in C.
echo $(( 0x10 )) $(( 010 )) $(( 10 ))
# Precedence, all in one expression.
echo $(( 2 + 3 * 4 - 6 / 3 ))
echo $(( (1 + 2) * (3 + 4) ))
# Inside double quotes it is one field, and it is not split or globbed.
echo "[$(( 1 + 1 ))]"
# In an assignment and in a loop, which is where a shell script uses it.
n=0
while test $n -lt 4; do
  n=$(( n + 1 ))
done
echo n=$n
# The line configure actually runs.
test $(( 1 + 1 )) = 2 && echo suitable
