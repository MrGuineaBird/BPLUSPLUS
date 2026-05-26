# B++

B++ is a beginner-friendly programming language that compiles `.bpp` files to Python. It is meant to feel like Scratch blocks written as text, with the simple `end` style of languages like Ruby and Lua.

It includes a command-line compiler, a Windows setup wizard, Linux user-local installation, `.bpp` file association on Windows, and GitHub Release based updates.

## Status

B++ version: `4.0`

B++ currently targets Windows and Linux.

The compiler emits Python source code and can also compile and run a `.bpp` program in one command.

## Installation

### Windows

Download the latest `B++ Setup.exe` from the Releases page:

```text
https://github.com/MrGuineaBird/BPLUSPLUS/releases
```

Run the installer and open a new terminal after setup finishes.

The installer adds B++ to your user `PATH` and registers `.bpp` files with Windows.

Installed commands:

```text
bpp
b++
```

### Linux

Clone or download the repository, then run:

```sh
python3 b++.py --install
```

This installs B++ for the current user:

```text
~/.local/share/bpp
~/.local/bin/bpp
~/.local/bin/b++
~/.local/bin/bpp-run
```

Make sure `~/.local/bin` is in your `PATH`.

## Quick Start

Create `hello.bpp`:

```bpp
set name to "B++"
say "Hello " + name
```

Run it:

```powershell
bpp --run .\hello.bpp
```

Compile it to Python:

```powershell
bpp .\hello.bpp
```

This creates:

```text
hello.py
```

Print the generated Python without running it:

```powershell
bpp --emit .\hello.bpp
```

## Running `.bpp` Files

After installation, `cmd.exe` can run `.bpp` files directly:

```cmd
hello.bpp
```

PowerShell is stricter with custom executable extensions, so use:

```powershell
bpp --run .\hello.bpp
```

On Linux, run B++ files through `bpp`:

```sh
bpp --run ./hello.bpp
```

Executable `.bpp` scripts can use `bpp-run`:

```bpp
#!/usr/bin/env bpp-run
say "Hello from B++"
```

Then run:

```sh
chmod +x hello.bpp
./hello.bpp
```

## Command Reference

```powershell
bpp file.bpp                 # compile to file.py
bpp --run file.bpp           # compile and run
bpp --emit file.bpp          # print generated Python
bpp --version                # show compiler version
bpp --install                # install B++ for the current user
bpp --uninstall              # remove the current-user install
bpp doctor                   # check the installation
bpp updates                  # show update settings
bpp check-update             # check GitHub for a newer release
bpp update                   # install the newest release
bpp --auto                   # enable automatic update checks
bpp --no-auto                # disable automatic update checks
bpp-run file.bpp             # Linux helper for executable .bpp scripts
```

Use `--` to pass arguments to a B++ program when running it:

```powershell
bpp --run .\app.bpp -- arg1 arg2
```

## Syntax Reference

B++ uses one statement per line. Blank lines are ignored. Comments start with `#`, except inside strings.

Blocks use `:` to start and `end` to close. Indentation is recommended for readability, but `end` controls the block.

### Values And Expressions

B++ expressions compile to Python expressions. This means strings, numbers, lists, dictionaries, math, comparisons, function calls, and indexing work like Python.

```bpp
"hello"
42
3.14
[1, 2, 3]
{"name": "B++"}
score >= 10
names[0]
```

Use Python's built-in constants:

```bpp
True
False
None
```

B++ also accepts simple lowercase values:

```bpp
true
false
nothing
nil
```

When `+` is used with a string, B++ converts both sides to text:

```bpp
say "score: " + score
```

### Comments

```bpp
# This line is ignored.
say "hello" # This part is ignored too.
```

### Output

`say value` prints a value.

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

`divide name by value` divides a variable. Whole-number results stay as whole numbers.

```bpp
divide score by 2
```

### Lists

Lists use Python-style list values.

```bpp
set names to []
```

`put value in list` adds a value to the end of a list.

```bpp
put "Ada" in names
```

`remove value from list` removes a value from a list.

```bpp
remove "Ada" from names
```

`empty list` removes everything from a list.

```bpp
empty names
```

### Input

`ask prompt into name` asks the user for text and stores the answer.

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

`repeat count times as name:` repeats and gives the current turn number, starting at `1`.

```bpp
repeat 3 times as turn:
    say turn
end
```

### For Each Loops

`for each name in list:` loops over each value in a list or other collection.

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

`return value` sends a value back from a function. `return` by itself returns `None`.

```bpp
def greet(name):
    return "Hi " + name
end
```

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

### Python Imports

`import module` imports a Python module.

```bpp
import math
say math.sqrt(16)
```

Advanced Python statements that are valid on one line can also pass through to the generated Python.

```bpp
from random import randint
say randint(1, 6)
```

### B++ Modules

`import "name"` imports a B++ module from `name.bpm` or `modules/name.bpm`.

```bpp
import "tools"
say tools.double(4)
```

Inside a `.bpm` file, `export name` changes the name used by the importer.

```bpp
export helpers

def double(value):
    return value * 2
end
```

Importing that module makes `helpers.double(...)` available.

## Build From Source

Requirements:

- Windows or Linux
- Python 3.11 or newer
- PyInstaller

Install PyInstaller:

```powershell
python -m pip install pyinstaller
```

Build the compiler:

```powershell
python .\build_bpp_installer.py
```

On Windows, the build also creates the setup wizard.

Build outputs on Windows:

```text
dist\bpp.exe
dist\B++ Setup.exe
dist\latest.example.json
```

Build outputs on Linux:

```text
dist/bpp
dist/latest.example.json
```

`latest.example.json` is a helper file for release metadata. The built-in updater uses GitHub Releases directly.

## Project Layout

```text
b++.py                  B++ compiler source
bpp_setup.py            Windows setup wizard
build_bpp_installer.py  release build script
dist/                   generated release artifacts
```

## Development

Run the compiler smoke test:

```powershell
python .\b++.py --self-test
```

Compile-check the Python sources:

```powershell
python -m py_compile .\b++.py .\bpp_setup.py .\build_bpp_installer.py
```

Run a B++ program from stdin:

```powershell
@'
say "Hello from stdin"
'@ | python .\b++.py --run -
```
