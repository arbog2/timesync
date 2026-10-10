@echo off
setlocal
REM LANTimeSync build script - locates MSVC + Windows SDK and builds x64 release.
REM Portable: resolves every path from the script's own location (%~dp0).

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "OUT=%ROOT%\bin"
set "EXE=%OUT%\timesync.exe"

REM ---- locate MSVC via vswhere ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found. Install VS Build Tools.
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo [ERROR] Visual Studio C++ workload not found.
  exit /b 1
)

REM ---- find the newest MSVC toolset ----
set "MTOOLS="
for /f "delims=" %%i in ('dir /b /ad "%VSDIR%\VC\Tools\MSVC" 2^>nul') do set "MTOOLS=%%i"
if not defined MTOOLS (
  echo [ERROR] No MSVC toolset under "%VSDIR%\VC\Tools\MSVC"
  exit /b 1
)
set "CL_EXE=%VSDIR%\VC\Tools\MSVC\%MTOOLS%\bin\Hostx64\x64\cl.exe"
set "LINK_EXE=%VSDIR%\VC\Tools\MSVC\%MTOOLS%\bin\Hostx64\x64\link.exe"
if not exist "%CL_EXE%"   ( echo [ERROR] x64 cl.exe missing & exit /b 1 )
if not exist "%LINK_EXE%" ( echo [ERROR] x64 link.exe missing & exit /b 1 )

REM ---- find the newest Windows SDK ----
set "SDKROOT=%ProgramFiles(x86)%\Windows Kits\10"
set "SDKVER="
for /f "delims=" %%i in ('dir /b /ad "%SDKROOT%\Include" 2^>nul') do set "SDKVER=%%i"
set "SDKINC=%SDKROOT%\Include\%SDKVER%"
set "SDKLIB=%SDKROOT%\Lib\%SDKVER%\um\x64"
if not exist "%SDKINC%\um\windows.h" ( echo [ERROR] Windows SDK headers missing & exit /b 1 )

REM ---- resource compiler ----
set "RC_EXE=%SDKROOT%\bin\%SDKVER%\x64\rc.exe"
if not exist "%RC_EXE%" set "RC_EXE=%SDKROOT%\bin\x64\rc.exe"
if not exist "%RC_EXE%" (
  echo [WARN] rc.exe not found, building without resources
  set "RC_EXE="
)

if not exist "%OUT%" mkdir "%OUT%"

REM Windows 7 SP1 target: 0x0601 = Win7 RTM/SP1, NTDDI 0x06010000.
REM /SUBSYSTEM:WINDOWS,6.01 + /OSVERSION:6.1 set the PE minimum OS fields.
set CFLAGS=/nologo /O1 /MT /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DNTDDI_VERSION=0x06010000
set LFLAGS=/nologo /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS,6.01 /OSVERSION:6.1 /ENTRY:mainCRTStartup
set LIBS=kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ws2_32.lib ntdll.lib ole32.lib comctl32.lib comdlg32.lib bcrypt.lib

echo [build] toolset %MTOOLS%  SDK %SDKVER%  arch x64

pushd "%ROOT%"
if defined RC_EXE (
  echo [build] compiling resources...
  "%RC_EXE%" /nologo /fo resource.res /i "%SDKINC%\um" /i "%SDKINC%\shared" src\resource.rc
  if errorlevel 1 goto :fail
)

echo [build] compiling sources...
"%CL_EXE%" %CFLAGS% /I"%SDKINC%\um" /I"%SDKINC%\shared" /I"%SDKINC%\ucrt" /I"%VSDIR%\VC\Tools\MSVC\%MTOOLS%\include" ^
  /c src\util.c src\config.c src\config_extra.c src\ntp.c src\timeset.c src\service.c src\task.c src\auth.c src\startup.c src\ntpserver.c src\watermark.c src\cguard.c src\window.c src\main.c
if errorlevel 1 goto :fail

if defined RC_EXE (
  "%LINK_EXE%" %LFLAGS% /LIBPATH:"%SDKLIB%" /LIBPATH:"%SDKROOT%\Lib\%SDKVER%\ucrt\x64" ^
    /LIBPATH:"%VSDIR%\VC\Tools\MSVC\%MTOOLS%\lib\x64" ^
    util.obj config.obj config_extra.obj ntp.obj timeset.obj service.obj task.obj auth.obj startup.obj ntpserver.obj watermark.obj cguard.obj window.obj main.obj resource.res ^
    /OUT:"%OUT%\timesync_new.exe" %LIBS%
) else (
  "%LINK_EXE%" %LFLAGS% /LIBPATH:"%SDKLIB%" /LIBPATH:"%SDKROOT%\Lib\%SDKVER%\ucrt\x64" ^
    /LIBPATH:"%VSDIR%\VC\Tools\MSVC\%MTOOLS%\lib\x64" ^
    util.obj config.obj config_extra.obj ntp.obj timeset.obj service.obj task.obj auth.obj startup.obj ntpserver.obj watermark.obj cguard.obj window.obj main.obj ^
    /OUT:"%OUT%\timesync_new.exe" %LIBS%
)
if errorlevel 1 goto :fail
popd

powershell -NoProfile -Command "Move-Item -Force '%OUT%\timesync_new.exe' '%EXE%' -ErrorAction Stop; Write-Host 'MOVE_OK'"
if errorlevel 1 (
  echo MOVE_FAILED - new binary kept at %OUT%\timesync_new.exe ^(old one may be running^)
  exit /b 0
)

del /q "%ROOT%\*.obj" "%ROOT%\resource.res" 2>nul
echo.
echo [OK] built %EXE%
exit /b 0

:fail
popd
echo [ERROR] build failed.
exit /b 1
