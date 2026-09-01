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
