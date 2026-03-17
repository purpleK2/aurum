# test break/continue

for i in 1 2 3 ; do
	echo $i
	break
done

for i in 1 2 3 ; do
	echo $i
	continue
	echo $i
done

for i in 1 2 3 ; do
	for j in 1 2 3 ; do
		echo "i : $i j : $j"
		continue 2
		echo "wrong"
	done
done
