#!/bin/sh
set -u

MODULES="test_math test_float test_cmath test_int test_dict test_list
         test_exceptions test_types test_stat test_posixpath test_os
         test_fileio test_io test_time test_json test_re test_subprocess
         test_threading"

echo "== m99pytest: CPython's own regression suite, on this machine =="
/bin/python3 -c "import sys; print('m99pytest: interpreter', sys.version)"

cd /tmp
for t in $MODULES; do
    echo "== m99pytest module: $t =="
    toybox timeout 1800 /bin/python3 -u -m test $t
    echo "== m99pytest exit $t: $? =="
done

echo "== m99pytest done =="
