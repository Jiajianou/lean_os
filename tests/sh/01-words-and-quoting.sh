# Words, quotes, and what survives them.
GREETING=hello
NAME='lean os'
echo $GREETING "$NAME"
echo "a'b" 'c"d' "e\"f"
echo 'literal $GREETING and `backtick`'
echo "double $GREETING"
echo one\ word
echo "trailing space "x
empty=""
echo "[$empty]"
echo [$empty]
mixed=a'b'c"d"
echo "$mixed"
echo "$GREETING"'-'"$NAME"
