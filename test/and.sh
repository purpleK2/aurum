# test && and ||

echo hi && echo hello1
echo hu || echo hello2
false && echo hello3
false || echo hello4
false || true && echo ll
echo $?
