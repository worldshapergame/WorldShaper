@echo off
REM Build if needed, then play. This is the file to double-click.
setlocal

if not exist "%~dp0build\bin\WorldShaper.exe" (
    call "%~dp0build.bat"
    if errorlevel 1 exit /b 1
)

cd /d "%~dp0build\bin"
WorldShaper.exe %*
endlocal
