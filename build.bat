@echo off
setlocal

where gcc >nul 2>nul
if %errorlevel%==0 (
    gcc bpp.c -O2 -Wall -Wextra -o bpp.exe
    if errorlevel 1 exit /b %errorlevel%
    gcc setup.c -O2 -Wall -Wextra -o "B++ Setup.exe" -mwindows -lshell32 -ladvapi32 -lole32
    exit /b %errorlevel%
)

where cl >nul 2>nul
if %errorlevel%==0 (
    cl /O2 /Fe:bpp.exe bpp.c
    if errorlevel 1 exit /b %errorlevel%
    cl /O2 /Fe"B++ Setup.exe" setup.c user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib
    exit /b %errorlevel%
)

echo No C compiler found. Install MinGW-w64, LLVM/Clang, or run this from a Visual Studio Developer Command Prompt.
exit /b 1
