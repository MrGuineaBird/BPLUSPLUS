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

B++ version: `4.0`

Current compiler:

- source: `bpp.c`
- command: `bpp`
- input: `.bpp`
- output: `.c`
- runtime: generated C

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

The `.bpp` file association currently compiles a `.bpp` file to C. Direct
double-click run mode is still being built for the native compiler.

## Quick Start

Create `hello.bpp`:

```bpp
set name to "B++"
say "Hello " + name
```

Compile it to C:

```sh
bpp hello.bpp -o hello.c
```

Compile the generated C:

```sh
cc hello.c -o hello -lm
```

Run it:

```sh
./hello
```

On Windows with MinGW:

```powershell
.\bpp.exe .\hello.bpp -o .\hello.c
gcc .\hello.c -o .\hello.exe
.\hello.exe
```

## Examples

The repository includes:

```text
example.bpp
examples/native_demo.bpp
```

Compile an example:

```sh
bpp example.bpp -o example.c
cc example.c -o example -lm
./example
```

## Command Reference

```text
bpp file.bpp -o file.c       compile B++ to C
bpp file.bpp                 print generated C to stdout
bpp --version                show compiler version
bpp --help                   show help
```

## Syntax Reference

B++ uses one statement per line. Blank lines are ignored. Comments start with
`#`, except inside strings.

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

`set name to value` stores a value.

```bpp
set name to "B++"
set score to 10
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

Use `elif condition:` for another condition and `else:` for the fallback.

```bpp
if score > 10:
    say "high"
elif score == 10:
    say "exact"
else:
    say "low"
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

`for each name in list:` loops over each value in a list.

```bpp
set names to ["Ada", "Bea", "Kai"]

for each name in names:
    say "hi " + name
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

`next loop` skips to the next turn of the nearest loop.

```bpp
if name == "":
    next loop
end
```

### Functions

`def name(args):` creates a function.

```bpp
def square(x):
    return x * x
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

## Native Boundaries

Native B++ does not support foreign-language imports or passthrough statements.
Code should be B++ code.

The native module system is still being designed.

## Project Layout

```text
bpp.c                     native B++ compiler
setup.c                   native Windows setup wizard
Makefile                  Unix-style build file
build.bat                 Windows build helper
build.sh                  shell build helper
example.bpp               example B++ program
examples/native_demo.bpp  native compiler demo
DESIGN.md                 language design rules
CHANGELOG.md              update history
```

## Development

Build the compiler:

```sh
make
```

Compile the demo:

```sh
./bpp examples/native_demo.bpp -o native_demo.c
cc native_demo.c -o native_demo -lm
./native_demo
```
