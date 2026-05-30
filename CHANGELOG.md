# Changelog

## B++ 4.3

B++ 4.3 adds Lua/Ruby-style tables for keyed data.

- Added table literals with `{ name = value }`.
- Added multi-line table literals.
- Added bracket lookup with `table["key"]`.
- Added numeric list lookup with `list[0]`.
- Added a `bpp.cmd` Windows command shim during setup for more reliable terminal command discovery.
- Fixed setup so it no longer writes user `PATHEXT`; older `.BPP` overrides are cleaned up during install/uninstall.

## B++ 4.2.1

B++ 4.2.1 adds the Ruby/Lua-style syntax pack, the built-in `os` module, and one-step run mode.

- Added Ruby/Lua-style assignment with `name = value`.
- Added `const name = value` with compile-time protection against later changes.
- Added `function`, `elseif`, `unless`, `until`, `for name in list`, range `for` loops, `break`, and `continue`.
- Added `not`, `%`, and `^` expression operators.
- Added Lua-style `--` comments.
- Added the built-in `os` module enabled with `<bpp unpackage os>`.
- Added OS commands for folders, files, environment variables, shell command capture, exit codes, process IDs, process killing, and sleeping.
- Added one-step run mode: `bpp file.bpp` now compiles through C and runs the program.
- Added `--emit-c` for printing generated C and `--exe` for building native executables.
- Added a generated `.bpp` file icon to the Windows setup wizard.

## B++ 4.2

B++ 4.2 improves native compiler errors.

- Added reserved name protection for variables, functions, function parameters, and loop variables.
- Reserved C/native runtime names such as `int`, `double`, `main`, and `bpp_*` are blocked before C code is generated.
- Reserved-name errors now explain the exact bad name and say to choose a different name.
- Added recovery for bad block headers so one reserved function or loop name does not cause noisy follow-up errors from the skipped block body.
- Documented reserved-name behavior in the README.
- Started the fast numeric compiler backend as unreleased/internal work.

## B++ 4.1

B++ 4.1 adds the native Windows update path for the C version of B++.

- Added `bpp_version.h` as the shared version file.
- Added native Windows GitHub Release update checks.
- Added `bpp check-update`.
- Added `bpp update`.
- Added `bpp updates`.
- Added `bpp --auto` and `bpp --no-auto`.
- Update checks are locked to `MrGuineaBird/BPLUSPLUS`.
- Windows updates download the release asset named exactly `bpp.exe`.
- Updated the Windows and Linux install instructions in the README.

## B++ 4.0

B++ 4.0 is the native switch. The old scripting-language compiler path was
removed, and the C compiler is now the main implementation.

### Native Compiler

- Added `bpp.c` as the main B++ compiler source.
- The compiler is written in C.
- `.bpp` files now compile to C source.
- The generated C includes the B++ runtime needed by the program.
- The command is now `bpp`.
- `bpp --version` reports the B++ compiler version.
- `bpp file.bpp -o file.c` writes generated C to a file.
- `bpp file.bpp` prints generated C to stdout.

### Removed Old Tooling

- Removed the old compiler source file.
- Removed the old setup wizard.
- Removed the old release build script.
- Removed generated app-bundler build output.
- Removed generated `dist` release artifacts from the repository.
- Removed docs for the old generated-source workflow.

### Language

- Kept the clear B++ command syntax:
  - `say`
  - `set`
  - `add`
  - `subtract`
  - `multiply`
  - `divide`
  - `ask`
  - `read`
  - `write`
- Kept lowercase beginner constants:
  - `true`
  - `false`
  - `nothing`
  - `nil`
- Kept `say value without newline`.
- Kept list commands:
  - `put value in list`
  - `remove value from list`
  - `empty list`
- Kept list literals like `["Ada", "Bea"]`.
- Kept conditionals with `if`, `elif`, `else`, and `end`.
- Kept loops:
  - `repeat count times:`
  - `repeat count times as name:`
  - `for each name in list:`
  - `while condition:`
  - `forever:`
- Kept loop controls:
  - `stop loop`
  - `next loop`
- Kept functions with `def`, `return`, and `end`.
- Kept comments using `#`.

### Build

- Added a root `Makefile`.
- Added `build.bat` for Windows compiler builds.
- Added `build.sh` for shell builds.
- Build output is `bpp` or `bpp.exe`.
- Added `make install` and `make uninstall` for Linux-style user installs.
- Added `setup.c`, a native Windows setup wizard.
- Windows builds now produce `B++ Setup.exe` when a Windows C compiler is available.
- The setup wizard installs `bpp.exe` to the current user's AppData folder.
- The setup wizard can add B++ to user `PATH`, create a `b++` alias, and register `.bpp` files.

### Examples And Docs

- Updated `README.md` for public native B++ usage.
- Updated `DESIGN.md` around the native-first direction.
- Added `examples/native_demo.bpp`.
- Updated `example.bpp` for the native compiler workflow.

### Still Being Built

- Native `.bpm` modules.
- Direct run mode.
- Linux release updater support.
