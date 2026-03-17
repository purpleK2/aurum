# regression test for a bug that happend on empty variable
# they were the same as "" but they should be considered as space or ignored

echo test $T test

# in some old tash2 version it's the opposite "" or '' does not work
echo "" test
echo '' test
echo "$T" test
