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

cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 exit /b 1

cmake --build "%~dp0build"
if errorlevel 1 exit /b 1

echo.
echo Built %CONFIG% into build\bin\WorldShaper.exe
endlocal
