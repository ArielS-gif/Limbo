@echo off
rem Starts OT6.5 with Limbo: the backend (on the host's PC), the game through its FortniteLauncher.exe, and Limbo
rem loaded into it as soon as it starts. Expects next to each other: "Fortnite (2252016-3240987)", this folder and,
rem on the host's PC, the backend (Limbo-Backend). A friend playing on someone else's backend writes
rem "<host address>:3551" into out\backend.txt: then no backend is started here and the patch pak points there.
setlocal
set "LIMBO=%~dp0"
for %%i in ("%LIMBO%..") do set "ROOT=%%~fi"
set "GAME=%ROOT%\Fortnite (2252016-3240987)"
set "BIN=%GAME%\FortniteGame\Binaries\Win32"
set "PATCHPAK=%GAME%\FortniteGame\Content\Paks\FortniteGame-WindowsClient_p.pak"
if not exist "%BIN%\FortniteLauncher.exe" (
  echo The OT6.5 build isn't next to Limbo: expected "%GAME%"
  goto end
)
if not exist "%LIMBO%out\injector.exe" (
  echo No out\injector.exe: build Limbo ^(build.bat^) or download a release.
  goto end
)

rem Whose backend: this PC's, or the one out\backend.txt names.
set "ADDRESS=127.0.0.1:3551"
if exist "%LIMBO%out\backend.txt" set /p ADDRESS=<"%LIMBO%out\backend.txt"
set "REMOTE=1"
echo %ADDRESS%| findstr /b /i "127.0.0.1 localhost" >nul && set "REMOTE="

if not exist "%PATCHPAK%" (
  echo Building the patch pak for %ADDRESS%...
  node "%LIMBO%tools\build-patch-pak.js" %ADDRESS%
  if not exist "%PATCHPAK%" (
    echo Could not build the patch pak - is Node.js installed?
    goto end
  )
)

if defined REMOTE (
  echo Playing on the backend at %ADDRESS%.
  goto launch
)
netstat -an | findstr /r /c:":3551 .*LISTENING" >nul && goto launch
set "BACKEND="
if exist "%ROOT%\Limbo-Backend\index.js" set "BACKEND=%ROOT%\Limbo-Backend"
if not defined BACKEND if exist "%ROOT%\Limbo-Backend-main\index.js" set "BACKEND=%ROOT%\Limbo-Backend-main"
if not defined BACKEND if exist "%ROOT%\Fortify-Alpha-main\index.js" set "BACKEND=%ROOT%\Fortify-Alpha-main"
if not defined BACKEND (
  echo No backend folder ^(Limbo-Backend^) next to Limbo.
  goto end
)
echo Starting the backend...
start "Limbo backend" cmd /k "cd /d "%BACKEND%" && node index.js"
for /l %%i in (1,1,20) do (
  netstat -an | findstr /r /c:":3551 .*LISTENING" >nul && goto launch
  timeout /t 1 /nobreak >nul
)
echo The backend is not listening on 3551 - check its window.
goto end

:launch
tasklist /fi "imagename eq FortniteClient-Win64-Shipping.exe" | findstr /i "FortniteClient" >nul && (
  echo The game is already running - close it first.
  goto end
)
echo Launching OT6.5...
start "" /d "%BIN%" "%BIN%\FortniteLauncher.exe" -epicapp=Fortnite -epicenv=Prod -epicportal -epiclocale=en-us -skippatchcheck
rem Limbo goes in as soon as the game starts: chat logs in right after the game's own login and needs it by then.
"%LIMBO%out\injector.exe" --wait 60

:end
pause
