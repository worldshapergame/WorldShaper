@echo off
REM Build WorldShaper. Double-click this, or run it from a terminal.
REM   build.bat            release-with-debug-info build
REM   build.bat debug      debug build with validation and sanitizer-friendly settings
setlocal

REM  build.bat clean     throw the build directory away first
REM
REM  Not paranoia. Ninja decided a source file was up to date when it was not, reported
REM  "no work to do", and left a stale executable behind - which meant a measurement was
REM  taken against code that had already been replaced. The binary prints the time it was
REM  compiled at startup so that is visible rather than silent, and this is the way out.
if /I "%~1"=="clean" if exist "%~dp0build" rmdir /s /q "%~dp0build"

set CONFIG=RelWithDebInfo
if /I "%~1"=="debug" set CONFIG=Debug

REM Locate the Visual Studio build tools without hard-coding a version.
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -property installationPath`) do set VSPATH=%%i
if not defined VSPATH (
    echo ERROR: Visual Studio build tools not found.
    echo Install "Desktop development with C++" from the Visual Studio Installer.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: could not set up the MSVC environment.
    exit /b 1
)

REM Ninja ships with Visual Studio; put it on PATH so CMake finds it.
set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

REM ...and so does CMake, which this script used to assume was installed separately.
REM
REM "Desktop development with C++" puts both of these in the same place, and the line above took
REM one of them and left the other -- so a machine with Visual Studio and nothing else got
REM `'cmake' is not recognized as an internal or external command`, which names no product, no
REM installer and nothing to do about it. Appended rather than prepended: somebody who has
REM installed CMake deliberately keeps the one they chose.
set "PATH=%PATH%;%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: CMake was not found, and Visual Studio does not appear to have brought its own.
    echo Either tick "C++ CMake tools for Windows" in the Visual Studio Installer, or install
    echo CMake from https://cmake.org/download/ and make sure it is on PATH.
    exit /b 1
)

REM The Vulkan SDK, which is the other thing a first build needs and the one nothing announces.
REM
REM Without it CMake stops inside `find_package(Vulkan REQUIRED COMPONENTS glslc)` with a message
REM about a missing package variable, twenty lines into a stack of CMake file names. The thing to
REM be told is the name of a download.
if defined VULKAN_SDK goto :have_vulkan
where glslc >nul 2>nul
if not errorlevel 1 goto :have_vulkan
echo ERROR: the Vulkan SDK was not found ^(no VULKAN_SDK in the environment, and no glslc on PATH^).
echo WorldShaper is a Vulkan 1.3 renderer and compiles its shaders with the SDK's glslc.
echo Install it from https://vulkan.lunarg.com/sdk/home#windows and open a NEW terminal
echo afterwards, so the installer's environment variables are in scope.
exit /b 1
:have_vulkan

cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 (
    echo.
    echo ERROR: CMake could not set the build up. If it is complaining about Vulkan or about a
    echo compiler it cannot find, and you have just installed one of them, run  build.bat clean
    echo so it looks again instead of reusing what it decided last time.
    exit /b 1
)

cmake --build "%~dp0build"
if errorlevel 1 exit /b 1

echo.
echo Built %CONFIG% into build\bin\WorldShaper.exe
endlocal
