# Functions: arguments, status, and what they can see.
greet() { echo "greet [$1] [$2] count=$#"; }
greet one "two three"
greet
status() { return 4; }
status; echo "ret=$?"
outer() { inner "$1"; }
inner() { echo "inner [$1]"; }
outer "a b"
counter=0
bump() { counter=$(echo $counter | tr 012 123); }
bump; bump
echo "counter=$counter"
shadow() { echo "positional inside [$1]"; }
set -- outer-arg
shadow inner-arg
echo "positional after [$1]"
