# test for loops

for i in 1 2 3 ; do
	echo $i
done

for i in 1 2 3 \
	4 5 6 
do
	echo $i
done

for i in 1 2 3\
4 5 6 
do
	echo $i
done

for i in "a b"c 'd' "$HOME" ; do
	echo "we have $i"
done
