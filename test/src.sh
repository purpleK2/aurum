# test . built in

if [ "$TEST" = "sourced" ] ; then
	export TEST=hello
	return 0
fi

export TEST="sourced"
. ./src.sh
echo $TEST $0
