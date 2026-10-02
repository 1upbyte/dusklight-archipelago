@echo off
REM Build Dusklight itself from the pinned checkout (dusklight/), to run the mod locally.
if not defined VCVARSALL set "VCVARSALL=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARSALL%" x64 >nul
cd /d "%~dp0..\dusklight"
cmake --preset windows-msvc-relwithdebinfo || exit /b 1
cmake --build --preset windows-msvc-relwithdebinfo --target dusklight
