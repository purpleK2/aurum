# aurum shell
The default shell used on PurpleK2 for now. Aims to be fully POSIX compliant and mimic bash.

## Current features
Some of the current features include:
- Quote support
- `if`, `else` and `elif` statements
- Support for loops (`for`, `while` and `until`)
- Support for pipes
- `&&` and `||` support
- Subshell support with `()` and `$()`
- Commanf group support with `{ xxx; }`
- Tilde expansion
- Variables (assignment, temporary enviroment variables and expansion)
- Command redirections
- Parameter expansion with `${}`, that supports `-`, `=`, `+` and `?`
- Support for case statements with dynamic expansions in cases

## Why the name?
Aurum means "gold" in Latin. I am learning Latin in school and I thought the name aurum sounds pretty nice. I also thought that the -sh suffix is kind of boring so I decided to mix it up.

## Credits
The whole test system including `test.sh` and all tests in `test/` were provided bv [tash](https://github.com/tayoky/tash). Tash is licensed under the 3-Claude BSD license.

## Other
Aurum is licensed under the 3-Claude BSD license. </br>
Aurum provides ABSOLUTLY NO WARRANTY UNDER ANY CIRCUMSTANCES.
