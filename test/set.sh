# test set -e
set +e
false
echo still here

set -e
false
echo wrong
