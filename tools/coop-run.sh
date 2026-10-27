#!/usr/bin/env bash
# One co-op test round on this PC (Git Bash): closes running copies of OT6.5, starts the host (coop1) and sends it
# into Close the Gate with hosting on, then starts a second copy (coop2), injects it and makes it join 127.0.0.1.
# The DLL logs: out/gameserver.log (host), out/instance2/gameserver.log (joiner); the game's own logs:
# %LOCALAPPDATA%/FortniteGame/Saved/Logs/FortniteGame.log (host), FortniteGame_2.log (joiner).
#   tools/coop-run.sh            full round
#   tools/coop-run.sh joiner     only restart the joiner (the host keeps running)
#   tools/coop-run.sh lobby      both stay in the main menu; then: lobbyplay on the host, lobbyplay on the joiner
#   tools/coop-run.sh solo       only the host, in the main menu (e.g. then: playmission LaunchTheRocket)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/../out"
logs="$LOCALAPPDATA/FortniteGame/Saved/Logs"
mode="${1:-full}"

bin="$(cygpath -w "$here/../../Fortnite (2252016-3240987)/FortniteGame/Binaries/Win32")"
launch() {  # name x -> pid (the same arguments as Coop-Test.ps1)
    local args="-epicapp=Fortnite -epicenv=Prod -epicportal -epiclocale=en-us -skippatchcheck -AUTH_LOGIN=$1@ot65.local -AUTH_PASSWORD=$1 -AUTH_TYPE=epic -windowed -ResX=960 -ResY=540 -WinX=$2 -WinY=0 -nosound"
    powershell.exe -NoProfile -Command "(Start-Process -FilePath '$bin\\FortniteClient-Win64-Shipping.exe' -ArgumentList '$args' -WorkingDirectory '$bin' -PassThru).Id" | tr -d '\r'
}
wait_for() {  # file pattern seconds
    for _ in $(seq 1 "$3"); do grep -q -E "$2" "$1" 2>/dev/null && return 0; sleep 1; done
    echo "timed out waiting for /$2/ in $1"; return 1
}

if [ "$mode" = full ] || [ "$mode" = lobby ] || [ "$mode" = solo ]; then
    taskkill //F //IM FortniteClient-Win64-Shipping.exe >/dev/null 2>&1
    sleep 3
    # Old logs would satisfy the waits below: moved aside (the game makes its own backups too).
    for f in "$logs/FortniteGame.log" "$logs/FortniteGame_2.log" "$out/gameserver.log" "$out/instance2/gameserver.log"; do
        [ -f "$f" ] && mv -f "$f" "$f.prev"
    done
    host=$(launch coop1 0)
    echo "host pid $host"
    # Injected as the game starts: the DLL lets the chat log in (the game does that right after its own login,
    # seconds in) and waits for the engine itself.
    sleep 1
    "$out/injector.exe" --pid "$host" | tail -1
    wait_for "$logs/FortniteGame.log" "Spawning Pin at map socket" 300 || exit 1
    wait_for "$out/gameserver.log" "ready:" 120 || exit 1
    if [ "$mode" = lobby ] || [ "$mode" = solo ]; then echo "host in the main menu"; [ "$mode" = solo ] && exit 0; else
    printf 'fix host on\nplay\n' > "$out/command.txt"
    wait_for "$out/gameserver.log" "net: (hosting|InitListen|CreateNamed|this world|no engine)" 240 || exit 1
    grep -E "net: hosting|chest guardians" "$out/gameserver.log" | tail -2
    fi
else
    # Only the joiner: the newest copy (it logs to FortniteGame_2.log).
    joiner_old=$(tasklist //FI "IMAGENAME eq FortniteClient-Win64-Shipping.exe" //FO CSV //NH | tr -d '"\r' | cut -d, -f2 | tail -1)
    [ -n "$joiner_old" ] && taskkill //F //PID "$joiner_old" >/dev/null 2>&1
    sleep 3
    for f in "$logs/FortniteGame_2.log" "$out/instance2/gameserver.log"; do [ -f "$f" ] && mv -f "$f" "$f.prev"; done
fi

joiner=$(launch coop2 960)
echo "joiner pid $joiner"
sleep 1
"$out/injector.exe" --pid "$joiner" | tail -1
wait_for "$logs/FortniteGame_2.log" "Spawning Pin at map socket" 300 || exit 1
wait_for "$out/instance2/gameserver.log" "ready:" 120 || exit 1
if [ "$mode" = lobby ]; then echo "joiner in the main menu"; exit 0; fi
printf 'join\n' > "$out/instance2/command.txt"  # the backend's list of hosted games, as the Play tab finds them
echo "joining"
