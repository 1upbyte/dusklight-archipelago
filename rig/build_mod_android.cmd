@echo off
REM Cross-build the mod for Android (arm64), matching what CI does.
REM Output: build-android\mods\archipelago.dusk (android-aarch64 only).
REM Set VCVARSALL, JAVA_HOME, ANDROID_HOME and ANDROID_NDK_VERSION to match your machine.
if not defined VCVARSALL set "VCVARSALL=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARSALL%" x64 >nul
if not defined JAVA_HOME set "JAVA_HOME=F:\Android\jdk"
if not defined ANDROID_HOME set "ANDROID_HOME=F:\Android\sdk"
if not defined ANDROID_NDK_VERSION set "ANDROID_NDK_VERSION=29.0.14206865"
cd /d "%~dp0.."
if not exist build-android\build.ninja cmake -B build-android -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE=%ANDROID_HOME%/ndk/%ANDROID_NDK_VERSION%/build/cmake/android.toolchain.cmake ^
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 || exit /b 1
cmake --build build-android --parallel %* 2>&1
