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
