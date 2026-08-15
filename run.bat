@echo off
REM Build, then play. This is the file to double-click.
REM
REM It used to build ONLY when build\bin\WorldShaper.exe was missing, which meant that the second
REM time anybody ran it -- after a git pull, after editing a shader, after any change at all -- it
REM started the executable that was already there and ignored everything new. "I updated and
REM nothing changed" is that, and it is indistinguishable from a change that did not work.
REM
REM So it always builds. Ninja does nothing when nothing changed, so the cost of that is under a
REM second, and build.bat's own header explains why a stale binary is worth this much care.
setlocal

REM A copy of the game that is still running holds the file the linker is about to write, and the
REM error for that is LNK1168, which reads like a fault in the code and is not one. Said here
REM rather than left to be decoded.
tasklist /fi "imagename eq WorldShaper.exe" 2>nul | find /i "WorldShaper.exe" >nul
if not errorlevel 1 (
    echo.
    echo WorldShaper is already running, so the build cannot replace its executable.
    echo Close that window first and run this again.
    echo.
    echo    ...or, to open a SECOND window without rebuilding, start the game directly:
    echo        build\bin\WorldShaper.exe
    echo.
    exit /b 1
)

call "%~dp0build.bat"
if errorlevel 1 (
    echo.
    echo The build failed, so the game was not started. The reason is above this line.
    exit /b 1
)

REM The three things the game needs beside its executable, named one at a time.
REM
REM Without this, a missing one is discovered by the game and reported as whatever it broke: no
REM shaders is a black window, and no clips is an empty worlds shelf and "this world built to
REM nothing" -- a sentence about a parser, for a file that was never there. Both are the same
REM question, "is the install complete", and it is cheaper to ask it here.
set "WS_BIN=%~dp0build\bin"
if not exist "%WS_BIN%\WorldShaper.exe" (
    echo ERROR: the build reported success but %WS_BIN%\WorldShaper.exe is not there.
    exit /b 1
)
if not exist "%WS_BIN%\shaders\visibility.comp.spv" (
    echo ERROR: no compiled shaders in %WS_BIN%\shaders.
    echo The build compiles them with glslc from the Vulkan SDK; if the SDK was installed after
    echo the build directory was created, run  build.bat clean  so CMake looks for it again.
    exit /b 1
)
if not exist "%WS_BIN%\clips\facility.clip" (
    echo ERROR: no clips in %WS_BIN%\clips, so there is no world to open.
    echo The build copies clips\ there ^(the ws_clips target^); if that folder is empty, run
    echo    build.bat clean
    exit /b 1
)

REM Run from beside the executable, which is where the game looks for its shaders, its clips and
REM the worlds shelf it seeds from them.
cd /d "%WS_BIN%"
WorldShaper.exe %*
endlocal
