while false ; do
	echo hello
done

until true ; do
	echo until work !
done

VAR=0

until [ $VAR = 2 ] ; do
	if [ $VAR = 0 ] ; then
		VAR=1
	elif [ $VAR = 1 ] ; then
		VAR=2
	fi
	echo "VAR is $VAR"
done
