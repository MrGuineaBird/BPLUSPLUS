# B++

B++ is a beginner-friendly programming language that compiles `.bpp` files to Python.

It includes a command-line compiler, a Windows setup wizard, `.bpp` file association, and GitHub Release based updates.

## Status

B++ currently targets Windows.

The compiler emits Python source code and can also compile and run a `.bpp` program in one command.

## Installation

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

## Command Reference

```powershell
bpp file.bpp                 # compile to file.py
bpp --run file.bpp           # compile and run
bpp --emit file.bpp          # print generated Python
bpp --version                # show compiler version
bpp doctor                   # check the installation
bpp updates                  # show update settings
bpp check-update             # check GitHub for a newer release
bpp update                   # install the newest release
bpp --auto                   # enable automatic update checks
bpp --no-auto                # disable automatic update checks
```

Use `--` to pass arguments to a B++ program when running it:

```powershell
bpp --run .\app.bpp -- arg1 arg2
```

## Language Examples

### Variables

```bpp
set score to 10
add 5 to score
subtract 2 from score
multiply score by 3
divide score by 2
say score
```

### Input

```bpp
ask "What is your name?" into name
say "Hi " + name
```

### Conditionals

```bpp
set age to 18

if age >= 18:
    say "adult"
else:
    say "minor"
end
```

### Loops

```bpp
repeat 3 times:
    say "B++"
end

set count to 0
while count < 3:
    say count
    add 1 to count
end
```

### Functions

```bpp
def square(x):
    return x * x
end

say square(5)
```

### Files

```bpp
write "hello" to "message.txt"
read "message.txt" into message
say message
```

### Python Imports

```bpp
import math
say math.sqrt(16)
```

## Updates

B++ updates are fixed to the official repository:

```text
https://github.com/MrGuineaBird/BPLUSPLUS
```

The update source cannot be changed from the installer or CLI.

The updater checks the latest GitHub Release, compares the release tag with the installed compiler version, downloads the `bpp.exe` release asset, and replaces the installed compiler.

Release tags should use version names such as:

```text
v5.2
```

Each release should include:

```text
bpp.exe
B++ Setup.exe
```

## Build From Source

Requirements:

- Windows
- Python 3.11 or newer
- PyInstaller

Install PyInstaller:

```powershell
python -m pip install pyinstaller
```

Build the compiler and installer:

```powershell
python .\build_bpp_installer.py
```

Build outputs:

```text
dist\bpp.exe
dist\B++ Setup.exe
dist\latest.example.json
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

## License

Add a license before publishing or distributing B++ publicly.
