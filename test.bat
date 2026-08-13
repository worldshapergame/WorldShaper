@echo off
REM Run the automated test suite.
REM These are the safety net: there is no second engineer reviewing this code, so a
REM failing test here is the difference between finding a bug now and finding it in a
REM world someone spent a month building.
setlocal

call "%~dp0build.bat" %*
if errorlevel 1 exit /b 1

"%~dp0build\bin\ws_tests.exe"
if errorlevel 1 (
    echo.
    echo TESTS FAILED
    exit /b 1
)

echo.
echo Running the headless world audit...
"%~dp0build\bin\WorldShaper.exe" --ticks 100000
if errorlevel 1 exit /b 1

echo.
echo Running the node pool audit...
REM `--stream-frames 300`, the chunk-mirror audit that stood here, went with chunk residency
REM in R1e -- and because an unknown argument is only a warning, this line stopped being a
REM test without saying so: it opened the game, sat on the title screen until somebody killed
REM it (66 minutes, twice), and the batch then printed "All tests passed" regardless. A stage
REM that cannot fail is not a test. The audits that replaced it run at every screenshot, so
REM this takes one -- small and cheap, off to one side, and with --no-clip-cache so running
REM the tests never advances the clip cache every renderer measurement is compared against.
set "WS_AUDIT_LOG=%TEMP%\ws_pool_audit.log"
"%~dp0build\bin\WorldShaper.exe" --screenshot "%TEMP%\ws_pool_audit.png" --screenshot-frame 60 ^
    --width 640 --height 400 --quality 4 --no-vsync --no-auto-quality --no-update-check ^
    --no-clip-cache --max-seconds 120 > "%WS_AUDIT_LOG%" 2>&1
if errorlevel 1 (
    type "%WS_AUDIT_LOG%"
    echo.
    echo NODE POOL AUDIT FAILED: the run itself did not finish
    exit /b 1
)
REM None of the three audits sets an exit code -- they log, because they run inside a normal
REM frame. So the pass is the presence of every phrase rather than the absence of a failure:
REM a run that crashed before the audit, or that the deadline cut off, prints none of them.
call :require "nodes.*GPU mirror matches" || exit /b 1
call :require "leaf for leaf" || exit /b 1
call :require "mask for mask" || exit /b 1
call :require "faces.*GPU mirror matches" || exit /b 1
del "%WS_AUDIT_LOG%" >nul 2>&1
del "%TEMP%\ws_pool_audit.png" >nul 2>&1

echo.
echo All tests passed.
endlocal
exit /b 0

:require
REM Not >nul: an audit that passed silently reads exactly like an audit that did not run,
REM which is the mistake this whole stage exists to stop making twice.
findstr /r /c:%1 "%WS_AUDIT_LOG%"
if errorlevel 1 (
    type "%WS_AUDIT_LOG%"
    echo.
    echo NODE POOL AUDIT FAILED: nothing in the run matched %1
    exit /b 1
)
exit /b 0
