# test functions
func () {
	echo hi
}

func
func

if false ; then
	func2 () (echo bad)
else
	func2 ()
	(echo good)
fi

# test func with arg
arg_func () {
	echo got "$1" as first arg
	echo got "$@" as full
	# check that the shell does not replace $0
	echo got "$0" as script name
	echo got $# as args count
}

arg_func hi
arg_func "hello   from func"
arg_func test1 "test   2"

# test func with return
func_ret () {
	echo yes
	return 2
	echo no
}
func_ret
echo $?
