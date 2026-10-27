# Starts one more copy of OT6.5 for co-op tests: windowed, logged in automatically as a test account (the backend
# creates an account the first time a new name logs in), without sound. Prints the new process id; inject it with
#   out\injector.exe --pid <id>
# once it reaches the lobby. Each copy's DLL logs to its own folder (out\gameserver.log, then out\instance2\, ...).
#
#   .\Coop-Test.ps1 -Name coop1 -X 0 -Y 0
#   .\Coop-Test.ps1 -Name coop2 -X 960 -Y 0
param(
    [string]$Name = "coop1",
    [int]$X = 0,
    [int]$Y = 0,
    [int]$Width = 960,
    [int]$Height = 540
)
$root = Split-Path (Split-Path $PSScriptRoot)
$bin  = Join-Path $root "Fortnite (2252016-3240987)\FortniteGame\Binaries\Win32"
$exe  = Join-Path $bin "FortniteClient-Win64-Shipping.exe"
$gameArgs = "-epicapp=Fortnite -epicenv=Prod -epicportal -epiclocale=en-us -skippatchcheck " +
            "-AUTH_LOGIN=$Name@ot65.local -AUTH_PASSWORD=$Name -AUTH_TYPE=epic " +
            "-windowed -ResX=$Width -ResY=$Height -WinX=$X -WinY=$Y -nosound"
$process = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $bin -PassThru
Write-Output $process.Id
