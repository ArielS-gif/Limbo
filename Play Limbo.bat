@echo off
rem Starts OT6.5 with Limbo: the backend (on the host's PC), the game through its FortniteLauncher.exe, and Limbo
rem loaded into it as soon as it starts. Expects next to each other: "Fortnite (2252016-3240987)", this folder and,
rem on the host's PC, the backend (Limbo-Backend). A friend playing on someone else's backend writes
rem "<host address>:3551" into out\backend.txt: then no backend is started here and the patch pak points there.
setlocal
title Limbo
echo.
echo  ___       ___  _____ ______   ________  ________
echo ^|\  \     ^|\  \^|\   _ \  _   \^|\   __  \^|\   __  \
echo \ \  \    \ \  \ \  \\\__\ \  \ \  \^|\ /\ \  \^|\  \
echo  \ \  \    \ \  \ \  \\^|__^| \  \ \   __  \ \  \\\  \
echo   \ \  \____\ \  \ \  \    \ \  \ \  \^|\  \ \  \\\  \
echo    \ \_______\ \__\ \__\    \ \__\ \_______\ \_______\
echo     \^|_______^|\^|__^|\^|__^|     \^|__^|\^|_______^|\^|_______^|
echo.
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
  call :node || goto end
  echo Building the patch pak for %ADDRESS%...
  node "%LIMBO%tools\build-patch-pak.js" %ADDRESS%
  if not exist "%PATCHPAK%" (
    echo Could not build the patch pak - see the messages above.
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

findstr /c:"\"mongoose\"" "%BACKEND%\package.json" >nul && (
  echo This backend is an old version that needs MongoDB. Get the new Limbo-Backend.zip from
  echo https://github.com/ArielS-gif/Limbo-Backend/releases and put it in place of "%BACKEND%".
  goto end
)

rem A freshly downloaded backend has no packages and no .env yet: install them and make a fresh one the first time.
call :node || goto end
if not exist "%BACKEND%\node_modules\dotenv-safe" (
  echo Installing the backend's packages ^(first time only^)...
  pushd "%BACKEND%"
  call npm install --no-audit --no-fund --loglevel=error
  popd
)
if not exist "%BACKEND%\node_modules\dotenv-safe" (
  echo Could not install the backend's packages - see the messages above.
  goto end
)
if exist "%BACKEND%\.env" goto envready
set "SECRET="
for /f "usebackq delims=" %%s in (`node -p "require('crypto').randomBytes(32).toString('hex')"`) do set "SECRET=%%s"
>"%BACKEND%\.env" (
  echo PORT=3551
  echo JWT_SECRET=%SECRET%
)
echo Made the backend's settings ^(.env^).
:envready

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
exit /b

rem Node.js runs the backend and builds the patch pak. Installs it with winget when it's missing.
:node
where node >nul 2>&1 && exit /b 0
if exist "%ProgramFiles%\nodejs\node.exe" goto nodepath
where winget >nul 2>&1 || goto nodemissing
echo Installing Node.js ^(Windows may ask for permission - click Yes^)...
winget install -e --id OpenJS.NodeJS.LTS --source winget --silent --accept-package-agreements --accept-source-agreements
if not exist "%ProgramFiles%\nodejs\node.exe" goto nodemissing
:nodepath
rem Installed but not on this window's PATH yet (it was just installed).
set "PATH=%ProgramFiles%\nodejs;%PATH%"
exit /b 0
:nodemissing
echo Node.js isn't installed - get it from https://nodejs.org and run this again.
exit /b 1
