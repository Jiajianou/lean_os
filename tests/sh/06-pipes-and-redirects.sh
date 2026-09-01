# Pipelines, and where output goes.
echo one two three | tr ' ' '\n'
echo a b c | while read x y z; do echo "read [$x][$y][$z]"; done
echo hi | cat | cat | cat
echo pipe-status | cat; echo "st=$?"
false | true; echo "last-stage=$?"
echo written > out.txt
echo appended >> out.txt
cat out.txt
cat < out.txt
{ echo group-a; echo group-b; } > grp.txt
cat grp.txt
for i in 1 2; do echo "loop $i"; done > loop.txt
cat loop.txt
echo to-stderr 2>&1 | cat
nosuchcommand_xyz 2>/dev/null || echo "stderr swallowed"
printf_missing() { echo no-op; }
echo one > a.txt; echo two > b.txt
cat a.txt b.txt
