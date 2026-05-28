# B++

B++ is a small general-purpose programming language with human-readable syntax.
It is designed to feel clear like text-based Scratch, structured like Ruby or Lua,
and strict enough to keep mistakes understandable.

B++ 4.0 is now native-first. The compiler is written in C and compiles `.bpp`
programs to C source code.

```text
B++ -> C -> native executable
```

## Status

B++ version: `4.2.1`

Current compiler:

- source: `bpp.c`
- command: `bpp`
- input: `.bpp`
- output: `.c`
- runtime: generated C
- fast path: numeric programs use a tokenizer, parser, AST, type inference, and direct C numbers

B++ does not require another scripting language to compile B++ code.

## Design Philosophy

B++ is clear.

B++ follows a few rules:

- Code should read like a direct command.
- Common actions should have one obvious spelling.
- Blocks use `end` so structure is visible.
- Errors should explain the mistake in B++ terms.
- Hidden magic should be avoided.
- Native speed should not make the language harder to read.

## Install On Windows

Requirements:

- Visual Studio Build Tools with **Desktop development with C++**, or MinGW-w64

Open **Developer Command Prompt for Visual Studio** or a terminal where your C
compiler works, then run:

```cmd
cd "C:\path\to\BPLUSPLUS"
build.bat
```

This builds:

```text
bpp.exe
B++ Setup.exe
```

Run the setup wizard:

```cmd
"B++ Setup.exe"
```

Keep the default options checked:

- Add B++ to user `PATH`
- Create `b++` command alias
- Register `.bpp` files with Windows

After setup finishes, open a new terminal and check:

```cmd
bpp --version
```

The setup wizard installs B++ for the current Windows user. By default it copies
`bpp.exe` to:

```text
%LOCALAPPDATA%\Bpp\bin
```

## GitHub Releases And Updates

B++ update checks are locked to:

```text
https://github.com/MrGuineaBird/BPLUSPLUS
```

Before making a release, bump the version in:

```text
bpp_version.h
```

Use a GitHub Release tag that matches the version:

```text
v4.1
```

Then build the Windows release files:

```cmd
build.bat
```

Upload these files to the GitHub Release:

```text
bpp.exe
B++ Setup.exe
```

The updater downloads the release asset named exactly `bpp.exe`. The setup
wizard is included for new installs and manual reinstalls.

Update commands:

```text
bpp check-update    check the latest GitHub Release
bpp update          download and install the newest bpp.exe
bpp updates         show update settings
bpp --auto          enable automatic update checks
bpp --no-auto       disable automatic update checks
```

Automatic checks run at most once per day. If a newer release exists, B++ prints
a notice telling the user to run `bpp update`.

## Install On Linux

Requirements:

- GCC or Clang
- `make`

Build and install for the current user:

```sh
make
make install
```

This installs:

```text
~/.local/bin/bpp
```

Make sure `~/.local/bin` is in your `PATH`. Then check:

```sh
bpp --version
```

To uninstall:

```sh
make uninstall
```

To install somewhere else, set `PREFIX`:

```sh
make install PREFIX=/usr/local
```

You may need `sudo` when installing outside your home folder.

## Build From Source

Install a C compiler first.

On Linux or macOS with `cc`, GCC, or Clang:

```sh
make
```

On Windows with MinGW or a Visual Studio Developer Command Prompt:

```bat
build.bat
```

This builds the compiler:

```text
bpp
```

or on Windows:

```text
bpp.exe
```

On Windows, `build.bat` also builds the native setup wizard:

```text
B++ Setup.exe
```

The compiler now supports direct run mode: `bpp file.bpp` compiles through C
behind the scenes and runs the program.

## Quick Start

Create `hello.bpp`:

```bpp
set name to "B++"
say "Hello " + name
```

Run it:

```sh
bpp hello.bpp
```

Behind the scenes, B++ still compiles through C. To keep the generated C file:

```sh
bpp hello.bpp -o hello.c
```

To build a native executable without running it:

```sh
bpp hello.bpp --exe hello
```

On Windows:

```powershell
.\bpp.exe .\hello.bpp
.\bpp.exe .\hello.bpp --exe .\hello.exe
```

## Examples

The repository includes:

```text
example.bpp
examples/fast_math.bpp
examples/native_demo.bpp
examples/ruby_lua_core.bpp
examples/os_module.bpp
examples/everything_showcase.bpp
```

Run an example:

```sh
bpp example.bpp
```

## Fast Numeric Backend

B++ now has a fast numeric compiler path for programs that only use numeric
code. That path reads the whole program, tokenizes expressions, builds an AST,
infers numeric variables, and emits direct C `double` values instead of boxed
runtime values.

This kind of program uses the fast backend automatically:

```bpp
set total to 0

repeat 5 times as i:
    add i * 2 to total
end

if total == 30:
    say total
end
```

The generated C uses plain numeric variables:

```c
double total = 0.0;
double i = 0.0;
```

Programs that use strings, lists, files, functions, or other dynamic features
still compile through the normal B++ runtime backend. This keeps B++ working
while the fast compiler grows feature by feature.

## Command Reference

```text
bpp file.bpp -o file.c       compile B++ to C
bpp file.bpp                 compile and run a B++ program
bpp file.bpp --emit-c        print generated C to stdout
bpp file.bpp --exe app       build a native executable
bpp --version                show compiler version
bpp --help                   show help
bpp check-update             check GitHub Releases for an update
bpp update                   install the newest bpp.exe release asset
bpp updates                  show update settings
bpp --auto                   enable automatic update checks
bpp --no-auto                disable automatic update checks
```

## Syntax Reference

B++ uses one statement per line. Blank lines are ignored. Comments start with
`#` or `--`, except inside strings.

Blocks start with `:` and close with `end`.

```bpp
if score > 5:
    say "high"
end
```

Indentation is recommended for readability, but `end` controls the block.

### Values

B++ supports strings, numbers, booleans, empty values, variables, function calls,
list literals, math, comparisons, `and`, and `or`.

```bpp
"hello"
42
3.14
true
false
nothing
nil
[1, 2, 3]
score >= 10
name == "Ada"
```

When `+` is used with text, B++ converts the values to text:

```bpp
say "score: " + score
```

### Comments

```bpp
# This line is ignored.
say "hello" # This part is ignored too.
-- Lua-style comments are also ignored.
```

### Output

`say value` prints a value with a newline.

```bpp
say "Hello"
say 2 + 3
```

`say value without newline` prints without moving to the next line.

```bpp
say "B" without newline
say "++"
```

### Variables

`set name to value` stores a value. Ruby/Lua-style assignment is also supported.

```bpp
set name to "B++"
set score to 10

name = "B++"
score = 10
```

`const name = value` creates a value that cannot be changed later.

```bpp
const max_score = 100
```

### Math Commands

`add value to name` adds onto a variable.

```bpp
add 5 to score
```

`subtract value from name` subtracts from a variable.

```bpp
subtract 2 from score
```

`multiply name by value` multiplies a variable.

```bpp
multiply score by 3
```

`divide name by value` divides a variable.

```bpp
divide score by 2
```

Expressions also support `not`, `%`, and `^`.

```bpp
say not ready
say score % 2
say 2 ^ 8
```

### Lists

Create a list with `[]` or a list literal.

```bpp
set names to []
set numbers to [1, 2, 3]
```

`put value in list` adds a value to the end of a list.

```bpp
put "Ada" in names
```

`remove value from list` removes the first matching value.

```bpp
remove "Ada" from names
```

`empty list` removes everything from a list.

```bpp
empty names
```

### Input

`ask prompt into name` asks for text and stores the answer.

```bpp
ask "What is your name?" into name
say "Hi " + name
```

### If Statements

`if condition:` runs a block when a condition is true.

```bpp
if score >= 10:
    say "winner"
end
```

Use `elif condition:` or `elseif condition:` for another condition and `else:`
for the fallback.

```bpp
if score > 10:
    say "high"
elseif score == 10:
    say "exact"
else:
    say "low"
end
```

`unless condition:` runs a block when a condition is false.

```bpp
unless ready:
    say "not ready"
end
```

### Repeat Loops

`repeat count times:` repeats a block a fixed number of times.

```bpp
repeat 3 times:
    say "B++"
end
```

`repeat count times as name:` repeats and gives the current turn number,
starting at `1`.

```bpp
repeat 3 times as turn:
    say turn
end
```

### For Each Loops

`for each name in list:` loops over each value in a list. `for name in list:`
is the Ruby/Lua-style spelling.

```bpp
set names to ["Ada", "Bea", "Kai"]

for each name in names:
    say "hi " + name
end

for name in names:
    say "hi " + name
end
```

### Range For Loops

`for name from start to end:` counts through a numeric range.

```bpp
for i from 1 to 10:
    say i
end

for i from 10 down to 1:
    say i
end

for i from 0 to 100 step 5:
    say i
end
```

### While Loops

`while condition:` repeats while a condition is true.

```bpp
set count to 0

while count < 3:
    say count
    add 1 to count
end
```

`until condition:` repeats while a condition is false.

```bpp
until ready:
    ask "ready?" into answer
    ready = answer == "yes"
end
```

### Forever Loops

`forever:` repeats until the program is stopped or `stop loop` runs.

```bpp
set count to 0

forever:
    add 1 to count
    say count
    if count == 3:
        stop loop
    end
end
```

### Loop Controls

`stop loop` exits the nearest loop.

```bpp
if done:
    stop loop
end
```

`break` is the Ruby/Lua-style spelling.

```bpp
if done:
    break
end
```

`next loop` skips to the next turn of the nearest loop.

```bpp
if name == "":
    next loop
end
```

`continue` is the Ruby/Lua-style spelling.

```bpp
if name == "":
    continue
end
```

### Functions

`def name(args):` creates a function. `function name(args):` is also supported.

```bpp
def square(x):
    return x * x
end

function cube(x):
    return x * x * x
end

say square(5)
```

`return value` sends a value back from a function.

```bpp
def greet(name):
    return "Hi " + name
end
```

`return` by itself returns `nothing`.

### Files

`write value to filename` writes text to a file, replacing existing contents.

```bpp
write "hello" to "message.txt"
```

`read filename into name` reads a file into a variable.

```bpp
read "message.txt" into message
say message
```

### Built-In OS Module

`<bpp unpackage os>` enables the built-in `os` module. The directive must be
the first non-empty, non-comment line in the script.

```bpp
<bpp unpackage os>

os current folder into cwd
os home folder into home
os temp folder into temp

os file exists "notes.txt" into found
os folder exists "build" into found
os list folder "." into files

os create folder "build"
os copy file "a.txt" to "build/a.txt"
os move file "old.txt" to "archive/old.txt"
os delete file "temp.txt"
os delete folder "old_build" recursively

os env "PATH" into path
os set env "BPP_MODE" to "dev"
os remove env "BPP_MODE"

os run "git status" into output
os exit code into code
os last error into error

os process id into pid
os sleep 500 milliseconds
```

Folder deletion is intentionally explicit: use `recursively` when deleting a
folder tree.

### Reserved Names

Some names are reserved by B++, C, or the native runtime. They cannot be used as
variables, function names, function parameters, or loop variables.

```bpp
set int to 5
```

```text
[Line 1] "int" is reserved.
    --> set int to 5
    expected: choose a different name
```

```bpp
def double(value):
    return value * 2
end
```

```text
[Line 1] "double" is reserved.
    --> def double(value):
    expected: choose a different name
```

## Native Boundaries

Native B++ does not support foreign-language imports or passthrough statements.
Code should be B++ code.

Built-in modules use B++ directives such as `<bpp unpackage os>`.

## Project Layout

```text
bpp.c                     native B++ compiler
bpp_version.h             shared version number
setup.c                   native Windows setup wizard
Makefile                  Unix-style build file
build.bat                 Windows build helper
build.sh                  shell build helper
example.bpp               example B++ program
examples/fast_math.bpp    fast numeric backend demo
examples/native_demo.bpp  native compiler demo
examples/ruby_lua_core.bpp Ruby/Lua-style syntax demo
examples/os_module.bpp     built-in os module demo
examples/everything_showcase.bpp full language showcase
DESIGN.md                 language design rules
CHANGELOG.md              update history
```

## Development

Build the compiler:

```sh
make
```

Run the demo:

```sh
./bpp examples/native_demo.bpp
```
