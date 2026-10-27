# Starts OT6.5 with Limbo: the backend (on the host's PC), the game through its FortniteLauncher.exe, and Limbo's DLL
# loaded into it as soon as it starts.
#
# FortniteGame-WindowsClient_p.pak (built by build-patch-pak.js from this game's own files) carries the game's
# DefaultEngine.ini pointed at the backend (http, port 3551) and its chat server (port 5222), so the client talks to
# them directly: no proxy, no certificates, and nothing written to the Saved\Config folder the current Fortnite uses.
#
# Expected next to each other:  Fortnite (2252016-3240987)\   this repository (Limbo)\   the backend (Limbo-Backend)\
# A friend playing on someone else's backend writes "<host address>:3551" into out\backend.txt: then no backend is
# started here and the patch pak points at that address.
$limbo    = Split-Path $PSScriptRoot
$root     = Split-Path $limbo
$game     = Join-Path $root "Fortnite (2252016-3240987)"
$bin      = Join-Path $game "FortniteGame\Binaries\Win32"
$patchPak = Join-Path $game "FortniteGame\Content\Paks\FortniteGame-WindowsClient_p.pak"
$injector = Join-Path $limbo "out\injector.exe"
$gameArgs = "-epicapp=Fortnite -epicenv=Prod -epicportal -epiclocale=en-us -skippatchcheck"
$backend  = @("Limbo-Backend", "Fortify-Alpha-main") | ForEach-Object { Join-Path $root $_ } |
            Where-Object { Test-Path (Join-Path $_ "index.js") } | Select-Object -First 1

function Test-Port($port) { [bool](Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue) }

if (-not (Test-Path $bin)) { Write-Host "The OT6.5 build isn't next to Limbo: expected $game"; exit 1 }

# Whose backend: this PC's, or the one out\backend.txt names.
$address = "127.0.0.1:3551"
$backendTxt = Join-Path $limbo "out\backend.txt"
if (Test-Path $backendTxt) {
    $line = (Get-Content $backendTxt -TotalCount 1).Trim()
    if ($line) { $address = $line }
}
$remote = -not ($address -match "^(127\.0\.0\.1|localhost)(:|$)")

if (-not (Test-Path $patchPak)) {
    Write-Host "Building the patch pak for $address..."
    node (Join-Path $PSScriptRoot "build-patch-pak.js") $address
    if (-not (Test-Path $patchPak)) { Write-Host "Could not build $patchPak (is Node.js installed?)"; exit 1 }
}
if ($remote) {
    Write-Host "Playing on the backend at $address."
} else {
    if (-not (Test-Port 3551)) {
        if (-not $backend) { Write-Host "No backend folder (Limbo-Backend) next to Limbo."; exit 1 }
        Write-Host "Starting the backend..."
        Start-Process cmd -ArgumentList "/k", "cd /d `"$backend`" && node index.js"
        foreach ($i in 1..20) { if (Test-Port 3551) { break }; Start-Sleep -Seconds 1 }
    }
    if (-not (Test-Port 3551)) { Write-Host "The backend is not listening on 3551 - check its window."; exit 1 }
}
if (Get-Process FortniteClient-Win64-Shipping -ErrorAction SilentlyContinue) {
    Write-Host "The game is already running - close it first."; exit 1
}

Write-Host "Launching OT6.5..."
Start-Process -FilePath (Join-Path $bin "FortniteLauncher.exe") -ArgumentList $gameArgs -WorkingDirectory $bin

# Limbo goes in as soon as the game starts: chat logs in right after the game's own login and needs the DLL's fix by
# then (the DLL waits for the engine by itself).
if (-not (Test-Path $injector)) { Write-Host "No $injector - build Limbo or download a release."; exit 1 }
$client = $null
foreach ($i in 1..60) {
    $client = Get-Process FortniteClient-Win64-Shipping -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($client) { break }
    Start-Sleep -Milliseconds 500
}
if ($client) {
    Start-Sleep -Seconds 1
    & $injector --pid $client.Id
} else {
    Write-Host "The game didn't start; nothing loaded."
}
