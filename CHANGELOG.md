# Changelog

## B++ 4.0

B++ 4.0 is a major update from B++ 3.0. It turns B++ into a more complete beginner-friendly language toolchain: a compiler that emits Python, a real command-line runner, Windows and Linux install support, update checks through GitHub Releases, and a larger simple syntax set inspired by Scratch, Ruby, and Lua.

### Language

- Added more text-based Scratch-style syntax while keeping Ruby/Lua-style `end` blocks.
- Added lowercase beginner constants:
  - `true`
  - `false`
  - `nothing`
  - `nil`
- Added `say value without newline`.
- Added list commands:
  - `put value in list`
  - `remove value from list`
  - `empty list`
- Added `for each name in list:` loops.
- Added `repeat count times as name:` loops, where the counter starts at `1`.
- Added `forever:` loops.
- Added loop controls:
  - `stop loop`
  - `next loop`
- Kept core B++ commands:
  - `say`
  - `set`
  - `add`
  - `subtract`
  - `multiply`
  - `divide`
  - `ask`
  - `read`
  - `write`
- Kept conditionals with `if`, `elif`, `else`, and `end`.
- Kept functions with `def`, `return`, and `end`.
- Kept comments using `#`.
- Kept Python-style expressions for strings, numbers, lists, dictionaries, comparisons, indexing, and function calls.

### Compiler And Runner

- B++ now compiles `.bpp` files into Python source.
- Added `bpp file.bpp` to compile a `.bpp` file into a `.py` file.
- Added `bpp --run file.bpp` to compile internally and run without leaving a `.py` file behind.
- Added `bpp --emit file.bpp` to print the generated Python without writing it.
- Added stdin support with `bpp --run -`.
- Added program argument passing with `--`.
- Added better compile errors for invalid B++ syntax.
- Added `bpp --self-test` for a compiler smoke test.
- Added `bpp doctor` to check the install.

### Modules And Imports

- Added Python imports through normal `import` statements.
- Added B++ module imports with `import "name"`.
- Added `.bpm` module support.
- Added `modules/` lookup support for B++ modules.
- Added `export name` for choosing the imported module name.

### Windows Support

- Added `B++ Setup.exe`, a native Windows setup wizard.
- The setup wizard installs B++ for the current Windows user.
- The setup wizard installs `bpp.exe` and `b++.exe`.
- Added Windows `PATH` setup.
- Added `.BPP` to `PATHEXT` so `cmd.exe` can run `.bpp` files directly.
- Added `.bpp` file association with Windows.
- Opening or double-clicking `.bpp` files on Windows runs them through B++.
- Added context actions for opening, compiling, and editing `.bpp` files.
- Updated setup text to make clear that it installs the Windows build of B++.

### Linux Support

- Added Linux user-local install support with `python3 b++.py --install`.
- Linux installs to:
  - `~/.local/share/bpp`
  - `~/.local/bin/bpp`
  - `~/.local/bin/b++`
  - `~/.local/bin/bpp-run`
- Added `bpp-run` for executable `.bpp` scripts using:

```bpp
#!/usr/bin/env bpp-run
```

- Added Linux uninstall support with `bpp --uninstall`.
- Added Linux install checks through `bpp doctor`.
- Updated the build script so Linux builds `dist/bpp`.

### Updates

- Added GitHub Release based update checks.
- Update source is locked to:

```text
MrGuineaBird/BPLUSPLUS
```

- Added `bpp updates` to show update settings.
- Added `bpp check-update` to check for the newest release.
- Added `bpp update` to install the newest release.
- Added automatic update checks with `bpp --auto`.
- Added `bpp --no-auto` to disable automatic update checks.
- Added configurable update intervals with `--update-interval-hours`.
- Windows updates look for a `bpp.exe` release asset.
- Linux updates look for a `bpp-linux`, `bpp-linux-x64`, or `bpp` release asset.

### Build And Release

- Added `build_bpp_installer.py` for release builds.
- Windows builds now produce:
  - `dist/bpp.exe`
  - `dist/B++ Setup.exe`
  - `dist/latest.example.json`
- Linux builds now produce:
  - `dist/bpp`
  - `dist/latest.example.json`
- Added SHA-256 metadata generation for release helper metadata.

### Documentation

- Rebuilt the README for public release.
- Added Windows install instructions.
- Added Linux install instructions.
- Added a full syntax reference.
- Added command reference documentation.
- Added build-from-source instructions.
