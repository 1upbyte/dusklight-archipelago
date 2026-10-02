@echo off
REM Build the mod (MSVC + Ninja). Set VCVARSALL if Visual Studio lives elsewhere.
if not defined VCVARSALL set "VCVARSALL=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARSALL%" x64 >nul
cd /d "%~dp0.."
if not exist build\build.ninja cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
cmake --build build --parallel %* 2>&1
