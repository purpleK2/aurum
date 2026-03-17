# test subshell substitution

echo $(ls ..)
echo $(echo "(")
echo $(echo \()
echo $(ec\ho hi\ s '${PATH}' "${PATH}")
echo $(   ( echo yes && echo it work )
)

echo $(echo hi)
echo $(uname -m)
echo $(realpath $(dirname $0))

# posix shell must always remove trayling newlines
echo "$(echo test1 test2)"
