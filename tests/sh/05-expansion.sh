# Parameter expansion, and command substitution.
NAME='lean os'
echo "${NAME}"
echo "len=${#NAME}"
unset u
echo "[${u:-default}]"
echo "[${u:+alternate}]"
set_me=""
echo "[${set_me:-dflt}][${set_me-dflt}]"
echo "[${u:=assigned}][$u]"
p=/a/b/c.txt
echo "${p##*/}"
echo "${p#/a/}"
echo "${p%.txt}"
echo "${p%%.*}"
echo "sub=$(echo inner)"
echo "nested=$(echo outer $(echo deep))"
echo "backtick=`echo tick`"
multi=$(echo one; echo two)
echo "[$multi]"
x=$(echo trailing)
echo "[$x]"
echo "status-in-sub=$(false; echo $?)"
pre=abc
echo "${pre}def"

# ---- M99: the special parameters are not variables --------------------
#
# `${*-word}` asks whether `$*` is SET, and this shell answered by
# looking in the variable table - where there is no entry called `*` - so
# it produced the default even with arguments in hand. CPython's
# Modules/makesetup opens its main loop with `for i in ${*-Setup}`, so it
# processed one file called `Setup`, which does not exist, and configure
# produced a Makefile with no modules in it and did not fail.
set -- alpha beta
for i in ${*-DEFAULT}; do echo "star: $i"; done
for i in ${@-DEFAULT}; do echo "at: $i"; done
echo "count: [${#-DEFAULT}] status: [${?-DEFAULT}] name-set: [${0+yes}]"
set --
for i in ${*-DEFAULT}; do echo "empty star: $i"; done
echo "count now: [${#}]"
# And the length form still means length, which is the ambiguity the
# character after the `#` resolves.
word=abcde
echo "len=${#word} argc=${#}"
