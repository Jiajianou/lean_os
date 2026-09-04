# Here-documents, expanded and not.
NAME=world
cat <<END
hello $NAME
second line
END
cat <<'END'
literal $NAME
END
cat <<-END
	indented and stripped
	still stripped
END
cat <<END > here.txt
into a file: $NAME
END
cat here.txt
while read line; do echo "got [$line]"; done <<END
alpha
beta
END

# ---- M99: the two here-document bugs configure found -------------------
#
# Neither was reachable from anything this project had written, and both
# are ordinary in a generated script.

# 1. A here-document on a line that also has `||` or `&&`, where the last
#    command is a bare assignment. The parser peeked one token ahead to
#    ask "is this a function definition?", and lexing that token crossed
#    the newline - which is where here-document bodies are collected. It
#    rolled the position back and not the collection, so the body was
#    read by `cat` AND then executed as script.
cat <<EOF || fail=1
first body
