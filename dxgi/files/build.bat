@echo off
REM Build script for dxgi-mirror
REM Run this from a "Developer Command Prompt for VS" or "x64 Native Tools Command Prompt"

echo Building dxgi-mirror...

cl /O2 /EHsc /W3 /DNDEBUG main.cpp /Fe:dxgi-mirror.exe /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Created dxgi-mirror.exe
    echo.
    echo Usage: dxgi-mirror.exe --source 0 --target 1
    echo        dxgi-mirror.exe --list
    echo        dxgi-mirror.exe --help
    del *.obj 2>nul
) else (
    echo.
    echo Build failed!
)
