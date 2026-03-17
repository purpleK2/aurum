# test async list

false &
wait $!
echo $?

exit 2 &
wait $!
echo $?

exit 5 &
wait
echo $?
