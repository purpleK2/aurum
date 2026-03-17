# test case

case test in
	wrong)
		echo wrong
		;;
	(test)
		echo yes
esac
echo $?

TEST=test2
CASE="t*t?"
case $TEST in
	"t*t?")
		echo wrong
		;;
	$CASE)
		echo right
		false
		;;
esac
echo $?
