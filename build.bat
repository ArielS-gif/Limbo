@echo off
rem Builds the 32-bit injector and game-server DLL for the OT6.5 client (FortniteClient-Win64-Shipping.exe is x86).
rem Needs Visual Studio 2022 or newer with "Desktop development with C++"; it is found with vswhere. A prompt that
rem already has the x86 compiler set up (a "x86 Native Tools" prompt, or GitHub Actions) is used as it is.
setlocal
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio not found: install it with "Desktop development with C++" & exit /b 1)
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio has no C++ tools: add "Desktop development with C++" & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || (echo vcvarsall failed & exit /b 1)

:build
cd /d "%~dp0"
if not exist out mkdir out

set CFLAGS=/nologo /std:c++17 /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN

cl %CFLAGS% /Fe:out\injector.exe /Fo:out\ src\injector\main.cpp /link /SUBSYSTEM:CONSOLE || exit /b 1
cl %CFLAGS% /LD /Fe:out\gameserver.dll /Fo:out\ src\gameserver\*.cpp /link || exit /b 1

echo Build OK
