# test input/output redirection

echo hello from file > test.txt
echo 'string
   litteral' >> test.txt
cat < test.txt

ls .. 1> test.txt
cat test.txt

{
	ls ..
	echo grouping test
} > test.txt

echo sep
cat test.txt
