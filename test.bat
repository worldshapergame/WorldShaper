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
echo Running the headless streaming audit...
"%~dp0build\bin\WorldShaper.exe" --stream-frames 300
if errorlevel 1 exit /b 1

echo.
echo All tests passed.
endlocal
