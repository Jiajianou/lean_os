# M99: exec and trap, the other two things CPython's configure needs.
#
# `exec 5>>config.log` is how configure opens the log every later `>&5`
# writes to, and `trap '... rm -f ...' 0` is how it cleans up and how it
# gets its own exit status right. Neither existed in this shell.
#
# The host's shell is the oracle, as everywhere in this directory.

# ---- exec: a redirection with no command outlives the command ---------
exec 3>&1
echo "down three" >&3
exec 3>&-
echo "three closed, one still open"

# ---- exec: fd 5, the way configure uses it ----------------------------
exec 5>exec5.tmp
echo "line via five" >&5
echo "second via five" >&5
exec 5>&-
cat exec5.tmp
rm -f exec5.tmp

# ---- exec: an ordinary redirection on an ordinary command is undone ---
echo before > redir.tmp
{ echo inside > redir.tmp ; } 
echo after-still-here
cat redir.tmp
rm -f redir.tmp

# ---- trap: listing, resetting, ignoring -------------------------------
trap 'echo caught-usr1' USR1
trap
trap - USR1
trap
echo traps-cleared

# ---- trap: EXIT, and $? inside it -------------------------------------
# Run in a child shell so that this fixture's own exit status is not the
# thing under test - what is under test is that the action runs at all
# and sees the right status.
sh -c 'trap "echo exiting with \$?" 0; (exit 0); true' 
sh -c 'trap "echo exiting with \$?" 0; false; exit 3'; echo "child said $?"

# ---- trap: the form configure writes ----------------------------------
sh -c '
tmpfile=trap-cleanup.tmp
echo hello > $tmpfile
trap "rm -f $tmpfile; echo cleaned" 0
echo body ran
'
test -f trap-cleanup.tmp && echo "LEFTOVER" || echo "the file is gone"
