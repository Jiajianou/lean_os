# Exit status, and the operators that read it.
false; echo "s=$?"
true; echo "s=$?"
true && echo and-taken
false && echo and-not-taken
false || echo or-taken
true || echo or-not-taken
true && false; echo "chain=$?"
false || true; echo "chain2=$?"
! false; echo "not=$?"
! true; echo "not2=$?"
nosuchcommand_xyz 2>/dev/null; echo "missing=$?"
(exit 5); echo "sub=$?"
if false; then echo no; fi; echo "if-empty=$?"
true; false; echo "seq=$?"
