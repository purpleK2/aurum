# test args and special var
echo $#
echo $?
echo $@
echo $1
echo $2

# $@ is a weird special case
for i in "$@" ; do
	echo "$i"
done
