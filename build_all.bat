@echo off
setlocal
cd /d "%~dp0"

REM Build both x64 and x86. Run from anywhere; scripts resolve via their own folder.
call "%~dp0build.bat"
if errorlevel 1 exit /b 1
call "%~dp0build_x86.bat"
if errorlevel 1 exit /b 1

echo.
echo [OK] all architectures built.
