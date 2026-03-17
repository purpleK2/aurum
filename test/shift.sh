# test shift builtin

func () {
	echo $# $0 $1
	shift
	echo exit status $?
	echo $# $0 $1 $2
	shift 2
	echo exit status $?
	echo $# $0 $1
	shift
	echo exit status $?
}

func a b c
func a "b c" d
