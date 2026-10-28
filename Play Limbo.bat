@echo off
rem Starts OT6.5 with Limbo: the backend (on the host's PC), the game through its FortniteLauncher.exe, and Limbo
rem loaded into it as soon as it starts. Expects next to each other: "Fortnite (2252016-3240987)", this folder and,
rem on the host's PC, the backend (Limbo-Backend). A small menu first. Whose backend (out\play-on.txt): by itself, a
rem friend's found on the network (Radmin VPN, a LAN; tools\discover.js), else this PC's own ("auto"); a friend's by
rem address; or always this PC's ("host"). The one chosen goes in out\backend.txt for Limbo's DLL, and the patch pak is
rem rebuilt whenever it changes. And whether new accounts play the homebase tutorial (out\tutorial.txt; the backend is
rem told before the game starts).
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
if not exist "%BIN%\FortniteLauncher.exe" (
  echo The OT6.5 build isn't next to Limbo: expected "%GAME%"
  goto end
)
if not exist "%LIMBO%out\injector.exe" (
  echo No out\injector.exe: build Limbo ^(build.bat^) or download a release.
  goto end
)
rem The patch pak can't be rebuilt while the game has it open.
tasklist /fi "imagename eq FortniteClient-Win64-Shipping.exe" | findstr /i "FortniteClient" >nul && (
  echo The game is already running - close it first.
  goto end
)
rem Node.js builds the patch pak (and runs the backend on the host's PC).
call :node || goto end

:menu
rem Whose backend: out\play-on.txt says "auto" (a friend's found on the network, the one with players online first,
rem else this PC's), "host" (this PC's) or a friend's address. Before it existed, out\backend.txt held that address.
set "ADDRESS=127.0.0.1:3551"
set "FOUND="
set "CHOICE=auto"
if not exist "%LIMBO%out\play-on.txt" if exist "%LIMBO%out\backend.txt" copy /y "%LIMBO%out\backend.txt" "%LIMBO%out\play-on.txt" >nul
if exist "%LIMBO%out\play-on.txt" set /p CHOICE=<"%LIMBO%out\play-on.txt"
set "MODE=friend"
if "%CHOICE%"=="" set "MODE=auto"
if /i "%CHOICE%"=="auto" set "MODE=auto"
if /i "%CHOICE%"=="host" set "MODE=host"
if "%MODE%"=="friend" for /f "usebackq delims=" %%a in (`node "%LIMBO%tools\build-patch-pak.js" --address "%LIMBO%out\play-on.txt"`) do set "ADDRESS=%%a"
if "%MODE%"=="auto" echo.
if "%MODE%"=="auto" echo  Looking for friends' games on your network...
if "%MODE%"=="auto" for /f "usebackq tokens=1,*" %%a in (`node "%LIMBO%tools\discover.js"`) do if not defined FOUND (
  set "ADDRESS=%%a"
  set "FOUND=%%b"
)
set "REMOTE=1"
echo %ADDRESS%| findstr /b /i "127.0.0.1 localhost" >nul && set "REMOTE="
set "TUTORIAL=skip"
if exist "%LIMBO%out\tutorial.txt" set /p TUTORIAL=<"%LIMBO%out\tutorial.txt"
if /i not "%TUTORIAL%"=="play" set "TUTORIAL=skip"
echo.
if "%MODE%"=="friend" echo  Playing on a friend's backend: %ADDRESS%
if "%MODE%"=="host" echo  Playing on this PC's backend: friends can join you.
if "%MODE%"=="auto" if defined FOUND echo  Joining a friend's game found on your network: %FOUND% - %ADDRESS%
if "%MODE%"=="auto" if not defined FOUND echo  No friend's game on your network yet: this PC hosts, and friends can join you.
if "%TUTORIAL%"=="skip" echo  New accounts skip the homebase tutorial.
if "%TUTORIAL%"=="play" echo  New accounts play the homebase tutorial.
echo.
echo    Enter   start the game
echo    J       join a friend by the address their Limbo window shows
echo    H       always host on this PC
if not "%MODE%"=="auto" echo    A       find a friend's game on the network by itself ^(Radmin VPN, LAN^)
if "%MODE%"=="auto" echo    R       look again
echo    T       turn the homebase tutorial on or off
echo.
set "PICK="
set /p "PICK=  > "
if /i "%PICK%"=="J" goto join
if /i "%PICK%"=="H" goto host
if /i "%PICK%"=="A" goto auto
if /i "%PICK%"=="T" goto tutorial
if not "%PICK%"=="" goto menu
goto start

:join
set "FRIEND="
set /p "FRIEND=  Your friend's address (like 26.12.34.56): "
if not "%FRIEND%"=="" >"%LIMBO%out\play-on.txt" echo %FRIEND%
goto menu

:host
>"%LIMBO%out\play-on.txt" echo host
goto menu

:auto
>"%LIMBO%out\play-on.txt" echo auto
goto menu

:tutorial
if "%TUTORIAL%"=="skip" (set "TUTORIAL=play") else (set "TUTORIAL=skip")
>"%LIMBO%out\tutorial.txt" echo %TUTORIAL%
goto menu

:start
rem Limbo's DLL talks to the same backend (out\backend.txt). The patch pak points the game at it; built again only when
rem the address changed.
>"%LIMBO%out\backend.txt" echo %ADDRESS%
node "%LIMBO%tools\build-patch-pak.js" --if-needed %ADDRESS%
if errorlevel 1 (
  echo Could not build the patch pak - see the messages above.
  goto end
)

if defined REMOTE goto tell
netstat -an | findstr /r /c:":3551 .*LISTENING" >nul && goto hosting
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
  netstat -an | findstr /r /c:":3551 .*LISTENING" >nul && goto hosting
  timeout /t 1 /nobreak >nul
)
echo The backend is not listening on 3551 - check its window.
goto end

:hosting
echo.
echo  Friends on your network ^(Radmin VPN, LAN^) find this game by themselves, or with J and one of these addresses:
node -e "for (const [name, list] of Object.entries(require('os').networkInterfaces())) for (const a of list) if (a.family === 'IPv4' && !a.internal) console.log('     ' + a.address + '   ' + name)"
echo  If Windows asks whether node or the game may use the network, allow it ^(private networks^).
echo.

:tell
rem The tutorial choice goes to the backend this player logs in to (it applies to this PC's accounts there).
where curl >nul 2>&1 || goto launch
curl -s -m 5 -X POST "http://%ADDRESS%/fortnite/api/game/v2/ot65/tutorial/%TUTORIAL%" >nul 2>&1 && goto launch
echo  Could not reach the backend at %ADDRESS%.
if defined REMOTE echo  Is your friend's backend running ^(their Limbo window^) and are you both on the same VPN?
echo.

:launch
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
