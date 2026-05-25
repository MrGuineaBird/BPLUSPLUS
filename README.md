# B++

B++ is a small programming language that compiles `.bpp` source files into Python.

The project includes:

- `b++.py` - the B++ compiler
- `bpp_setup.py` - the Windows setup wizard
- `build_bpp_installer.py` - builds the release executables
- `dist/bpp.exe` - the packaged B++ compiler
- `dist/B++ Setup.exe` - the Windows installer

## Install

Run:

```powershell
& ".\dist\B++ Setup.exe"
```

You can also double-click `dist\B++ Setup.exe`.

The setup wizard installs B++ for the current Windows user. It adds B++ to `PATH`, registers `.bpp` files, and installs:

```text
%LOCALAPPDATA%\Bpp\bin\bpp.exe
%LOCALAPPDATA%\Bpp\bin\b++.exe
```

After installing, open a new terminal.

## Usage

Compile a B++ file to Python:

```powershell
bpp program.bpp
```

Compile and run:

```powershell
bpp --run program.bpp
```

Print generated Python:

```powershell
bpp --emit program.bpp
```

Check the install:

```powershell
bpp doctor
```

Check version:

```powershell
bpp --version
```

In `cmd.exe`, installed `.bpp` files can be run directly:

```cmd
program.bpp
```

PowerShell is stricter with custom executable extensions, so use:

```powershell
bpp --run .\program.bpp
```

## Example

Create `hello.bpp`:

```bpp
set name to "B++"
say "Hello " + name
```

Run it:

```powershell
bpp --run .\hello.bpp
```

Output:

```text
Hello B++
```

## Language Basics

Variables:

```bpp
set score to 10
add 5 to score
subtract 2 from score
multiply score by 3
divide score by 2
say score
```

Input:

```bpp
ask "What is your name?" into name
say "Hi " + name
```

Conditionals:

```bpp
set age to 18

if age >= 18:
    say "adult"
else:
    say "minor"
end
```

Loops:

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

Functions:

```bpp
def square(x):
    return x * x
end

say square(5)
```

Files:

```bpp
write "hello" to "message.txt"
read "message.txt" into message
say message
```

Python imports are also supported:

```bpp
import math
say math.sqrt(16)
```

## Updates

B++ updates are locked to this GitHub repository:

```text
https://github.com/MrGuineaBird/BPLUSPLUS
```

Users cannot change the update source from the CLI or installer.

Check update settings:

```powershell
bpp updates
```

Check for a newer release:

```powershell
bpp check-update
```

Install the newest release:

```powershell
bpp update
```

Enable automatic periodic update checks:

```powershell
bpp --auto
```

Disable automatic update checks:

```powershell
bpp --no-auto
```

Automatic updates check GitHub Releases for a newer release and download the `bpp.exe` asset from the latest release.

For releases, upload at least:

```text
bpp.exe
B++ Setup.exe
```

The release tag should be versioned like:

```text
v5.2
```

## Build

Install PyInstaller:

```powershell
python -m pip install pyinstaller
```

Build both executables:

```powershell
python .\build_bpp_installer.py
```

Outputs:

```text
dist\bpp.exe
dist\B++ Setup.exe
dist\latest.example.json
```

`latest.example.json` is a helper manifest example. The built-in updater uses GitHub Releases directly.

## Developer Commands

Run compiler smoke test:

```powershell
python .\b++.py --self-test
```

Compile-check the Python files:

```powershell
python -m py_compile .\b++.py .\bpp_setup.py .\build_bpp_installer.py
```

Run from stdin:

```powershell
@'
say "Hello from stdin"
'@ | python .\b++.py --run -
```
