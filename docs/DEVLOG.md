# OTGameServer

A DLL for the OT6.5 (Feb 2016, 32-bit, UE 4.12) Fortnite client, meant to eventually let the client host a mission
itself, so the backend's matchmaking has a session to hand out.

## Layout

- `build.bat` — builds both targets as 32-bit with Visual Studio 2026 (needs the Windows 11 SDK component).
- `src/injector/` — `injector.exe`: loads `gameserver.dll` into the running OT6.5 client. It only accepts a
  32-bit `FortniteClient-Win64-Shipping.exe` running from the `Fortnite (2252016-3240987)` folder.
- `src/gameserver/` — `gameserver.dll`.
  - `memory.*` — readable-region cache and SEH-protected reads, so bad pointers can't crash the game.
  - `ue.*` — UE 4.12 32-bit layouts, plus finders for the name table (GNames) and object table (GObjects).
  - `dump.*` — writes the tables to text, checks other projects' offsets, lists the mission-related classes.
  - `hook.*` — a small inline detour (checks the exact bytes first, pauses the other threads while patching).
  - `trace.*` — the ProcessEvent hook: logs the mission machinery and runs commands from `out\command.txt`
    (list in `trace.h`).
  - `engine.*` — shared engine access (lookups, StaticLoadObject, SpawnActor, FMemory, code patches).
  - `fixes.*` — keeps the logged-in user on world requests (step 3).
  - `game.*`, `mission.h`, `atlas.cpp`, `objectives.cpp`, `director.cpp`, `player.cpp` — the mission server (step 4).
- `tools/`
  - `Play-OT65-Direct.ps1` (+ `..\..\Play OT6.5 (no Fiddler).bat`) — starts the backend and the game with no
    Fiddler and no edits to the Saved\Config folder the current Fortnite shares.
  - `build-patch-pak.js` — builds `FortniteGame-WindowsClient_p.pak` next to the game's main pak: the game's own
    DefaultEngine.ini with the backend address (`[OnlineSubsystemMcp.BaseServiceMcp]`), `n.VerifyPeer=false` and
    verbose LogOnline/LogOnlineGame/LogFort. Delete that pak to undo.
  - `pak.js` — list/extract/create UE 4.12 (v3) paks; `backend-trace.js` — runs the backend with a request log.
  - `re.js` — static analysis of the exe: strings, references, functions, vtables, disassembly via dumpbin,
    annotated with native function names (needs `out\re\natives.txt`, from the `natives` trace command).
- `config/` — the game's original DefaultEngine.ini and the patched one that goes into the patch pak.
- `out/` — build output, `gameserver.log`, `seen.log`, `dump/`, `re/` (natives.txt, classes.txt), `runs/` (logs of
  each experiment).

## Step 1 (done 2026-10-26): map the engine

1. Start the game normally (`Play OT6.5.bat`).
2. Run `out\injector.exe` once the game has loaded (memory settles around 1.1 GB on the login screen).
3. Read `out\gameserver.log` and the files in `out\dump\`.

Results on the login screen (`/Game/Maps/FortniteEntry`):

- GObjects at exe+0x2DC3AAC (12-byte items, ~186k objects), GNames pointer at exe+0x2D87358 (~192k names).
- **Blaze's SDK and Alpharium's offsets fit this exe**: GObjects and ProcessEvent (exe+0x9A9E70, vtable slot 0x34)
  match, and GWorld (exe+0x2E68BEC) points at the current world. Only Blaze's GNames (exe+0x2DBC5A8) differs; its
  SDK names objects through FName::AppendString and calls that GNames value a fallback, so ours is the one to use.
- **There is no FortGameModeZone in this build, and nothing is missing because of it.** The mission rules live in
  `FortGameMode` itself (it holds MissionManagerClass, bWorldIsReady, bTheaterDataIsReady, TheaterSlot,
  CurrentZoneInstanceId, ZoneIndex). Its only subclasses are EmptyDedicated and FrontEnd.
- The Zone classes a mission runs with are all there: FortGameStateZone (+ Keep, Manor, Outpost, PvP,
  PvPBaseDestruction), FortPlayerControllerZone (+ ARPG, Keep, Manor, Outpost, PvP, ZoneFake, PvPFake),
  FortPlayerStateZone (+ the same family), FortMissionManager, FortMissionGenerationManager, FortWorldManager,
  FortAIDirector, FortGameSessionDedicated.
- The pak has the mission maps: `Maps/Zones/Zone_Temperate_{Forest,Grassland,Industrial,Suburban,Urban}`,
  `Zone_Onboarding_FarmsteadFort`, `Zone_Outpost_PvE_01..04`, plus `AITestbed_2`, `PVP/PVP_Tower` and the
  3x3/5x5 tiles the zones are built from.

So the open question is not a missing class but whether FortGameMode's server-side code paths (world building,
mission generation) survived in this client build or were compiled out.

## Reference projects (read, not run)

- **Blaze** (github.com/tr1268-v2/BlazeButBetter) — a Dumper-7 SDK of this exact exe: every class, struct and
  function with offsets. The most useful of the three; use it for field offsets and function parameters.
- **Alpharium** (github.com/ProjectEternal/Alpharium) — loads `AITestBed_2` or `PvP_Tower` standalone with
  `?Game=FortniteGame.FortGameMode`, swaps the Zone controller/state/game state in through a SpawnActor hook
  (exe+0x1352D70), then forces the match to start and spawns the pawn by hand. A sandbox, not a mission.
- **OT6-Reconstruction** (github.com/ApfelTeeSaft/OT6-Reconstruction) — real log strings from the exe with
  plausible code written around them (it targets Win64, which this build isn't). Good for names and log text to
  search for; don't trust its logic or member layouts.

## Step 2 (done 2026-10-26): load a zone and watch

`gameserver.dll` now hooks ProcessEvent (`hook.*`, a 9-byte detour that refuses to patch if the bytes differ)
and `trace.*` logs the mission machinery and takes commands from `out\command.txt`; see `trace.h` for the list.
The dump from step 1 is now the `dump` command. Logs of the two runs are in `out\runs\`.

1. Start the game, inject, wait for `ready:` in `gameserver.log`.
2. `travel /Game/Maps/Zones/Zone_Temperate_Forest?game=/Script/FortniteGame.FortGameMode`
3. Wait for `bWorldIsReady 1` (about 15 s), then `pc ServerSetClientHasFinishedLoading 1` and
   `pc ServerReadyToStartMatch`.

What the client does on its own:

- **Without `?game=`** the zone gets the plain engine GameMode/GameState/PlayerController: only the border,
  outer forest and skybox exist, nothing builds the middle. The zone maps don't pick a game mode themselves.
- **With FortGameMode, the client builds the zone itself.** The theater data is ready at once, the world
  manager places 3x3 and 5x5 building tiles with cliffs, props, chests and audio between them (a different
  layout each run), creates FortPlayerStarts, sets bWorldIsReady and calls the level's OnWorldReady. The server
  code for world building was not compiled out of this client.
- It then waits at MatchState `WaitingToStart`, the player held as a spectator, because nobody says the client
  is ready. Sending the two calls a real client sends is all it takes: after `ServerReadyToStartMatch`,
  ReadyToStartMatch turns true and **the game's own code** spawns the FortMissionManager, picks a player start,
  spawns `PlayerPawn_Generic_C`, sets `InProgress`, calls the level's OnMatchStarted, and BluGloManager starts
  receiving mission events. Nothing is forced (compare Alpharium, which does all of this by hand).

Confirmed offsets (FortGameMode MatchState 0x240, MissionManagerClass 0x360, ready bits 0x37C; FortGameState
MissionManager 0x2CC, WorldManager 0x328; FortPlayerController flags 0x4A4/0x510/0x511) read sensibly live.

Still open:

- The mission manager has no mission: CurrentZoneInstanceId is empty, GameplayState stays 0, and there is no
  theater/mission/zone theme behind it. In the real flow these come from the backend (`world/info`,
  `world/validate`) and the matchmaking session.
- The classes are the base ones (FortPlayerController, FortGameState), not the Zone ones; how the real flow
  picks FortPlayerControllerZone/FortGameStateZone is unknown.
- bHasServerFinishedLoading and bClientPawnIsLoaded stay 0.
- The travel was made from the login screen, so the login UI stayed on top of the zone.

## Step 3 (in progress): how a mission reaches a server

Tools added: `tools\backend-trace.js` runs the Fortify-Alpha backend unchanged and logs every request with its
body and response (secrets blanked) to `out\backend-trace.log`:
`node tools\backend-trace.js ..\Fortify-Alpha-main out\backend-trace.log` (start it before `Play OT6.5.bat`, which
then skips starting its own backend). New trace commands: `cheat`, `console` (e.g. `console flushlog`, because the
engine log `%LOCALAPPDATA%\FortniteGame\Saved\Logs\FortniteGame.log` is written in large batches), `missiongen`,
`missiongen-next`; `state` now shows the cheat manager, the mission list and the zone's world record.
Logs: `out\runs\step3-matchmaking`, `out\runs\step3-missiongen`.

### What Start Mission does (logged in, Stonewood, "Close the Gate")

1. Qos: `matchMakingRequest` with `GAMEMODE_s=QOSSERVER`; none, so the region defaults to USA.
2. `FindGatheringSession`: a session already running this mission, by
   `ZONEINSTANCEID_s = {"worldId":"","theaterId":"33A2311D...","theaterMissionId":"24s9ag75-...",
   "zoneThemeClass":"/Game/World/ZoneThemes/ZT_TheForest/BP_ZT_TheForest.BP_ZT_TheForest_C"}`, plus
   `REGION_s`, `MMLVL_i`, `NEEDS_i>=1`, `GATHERABLE_b`, `openPlayersRequired:1`.
3. `StartGatheringSession`: an empty dedicated server, `GAMEMODE_s=FORTEMPTY`, `requireDedicated`,
   `maxCurrentPlayers:0`.
4. Both lists are empty, "No matches available", repeat. The matchmaking states in the exe show what would come
   next: Acquiring Lock, Testing Empty Servers (party beacon reservation: Reservation Accepted/Denied/...),
   Joining Existing Session, Join Success. The server half is in the exe too (FortPartyBeaconHost,
   FortLobbyBeaconHostPvE, FortGameModeEmptyDedicated, `/api/game/v2/world/validate?sessionId=`).

So Epic ran idle servers in "empty" mode; a client reserved one, the server loaded the mission's zone, and the
client joined. Nothing has ever registered a session with the backend, hence the error.

### Where the mission data lives

- `world/validate`'s answer (missionGenerator, zoneDifficultyInfo, tileIndex) is `FFortZoneInstanceDetails`,
  stored in `FortWorldManager.CurrentWorldRecord` (0x41C) as `UFortWorldRecord.ZoneInstanceInfo` (0xE4).
- In a zone loaded with `?game=FortGameMode` that record is empty (no world/theater/mission/theme/generator), the
  world manager never contacts the backend, and no FortMissionGenerationManager exists. The mission manager is
  created at match start but `Missions` stays empty.
- Writing `MissionGen_1Gate` into the record, after the world is ready or 0.17 s into the load
  (`missiongen-next`, WorldManagerState 2 = WorldQueryComplete), changes nothing: the generator is not picked up
  from the record by itself. Something native in the real server flow (after world/validate) sets up mission
  generation; that is the next thing to find.
- The cheat `LoadMissionGenerator` does nothing in this build (its name list, `ListMissionGenerators`, is empty).
- Backend data bug (FIXED 2026-10-26 with `tools\fix-world-info.js`): `common/world_info.json` reused GUIDs: a
  made-up `24s9ag75-...` for Stonewood tiles 6/7/11, another shared pair (tiles 16/19) in both of the first two
  theaters, and those two theaters reusing each other's GUIDs. `world/validate` looks missions up by GUID, so it
  returned DeliverTheGoods for "Close the Gate". The script gave the 61 affected missions stable GUIDs
  (md5 of theater, tile and generator); all 150 are now unique, the rest of the file is unchanged, the original is
  `world_info.original.json`. Close the Gate (Stonewood tile 11, `MissionGen_1Gate`) is now
  `240487D1CE39E94760999521942A2B37`; validate returns MissionGen_1Gate for it. The backend reads the file at
  startup; the client reads world info at login.

### The config files, Mercury, and why missions never start (2026-10-26)

- The game's own DefaultEngine.ini says `GlobalDefaultGameMode="/Script/FortniteServer.FortGameModeZone"`,
  `GlobalDefaultServerGameMode=FortGameModeEmptyDedicated`, `ServerDefaultMap=/Game/Maps/FortniteEmptyDedicated`.
  FortGameModeZone lived in the server-only FortniteServer module, which is not in the client exe. That module is
  what turned a reserved empty server into a mission: it is why world/validate is never called and no
  FortMissionGenerationManager ever exists in our zones.
- `DefaultEngine.ini` / `FortniteServer.pak` (the copies in the OT6.5 folder) are a community client mod (the pak
  also points XMPP at vops' server): verbose logging, `n.VerifyPeer=false`, and
  `[OnlineSubsystemMcp.BaseServiceMcp] Protocol="http" Domain=...`, which sends every MCP service to one address.
  OT6.5 only loads extra paks named `FortniteGame-WindowsClient_p.pak` (MercuryLauncher's finding), so a pak
  named FortniteServer.pak is probably ignored. Our `build-patch-pak.js` uses the same redirect; tested: the
  client's startup requests reach the backend directly, no Fiddler, no -httpproxy.
- MercuryLauncher's OT6.5 mode uses no DLL: a `_p` patch pak (or loose files) with
  `GameDefaultMap=/Game/Maps/PVP/PVP_Tower`, `GlobalDefaultGameMode=/Game/InGame_Gamemode.InGame_Gamemode_C` and the
  same redirect to 127.0.0.1:3551. InGame_Gamemode is a blueprint subclass of FortGameModeEmptyDedicated with the
  Zone PC/PS/GameState/HUD classes; InGame_Actor gives all items, calls ServerReadyToStartMatch/StartMatch/Possess
  and spawns `/Game/Missions/Secondary/HuskHarvester/Mission_HuskHarvester` directly. A sandbox, not missions,
  but it shows mission actors can be spawned directly.
- The mission cheats are compiled out: FortCheatManager LoadMissionGenerator, LoadMission, SendMissionEvent,
  GiveAmmo, SpawnAIGroup and three more share one exec thunk (00A41B40) whose body (00861550) only frees the string.

### First real mission, half-way (2026-10-26 afternoon)

From two verified analysis workflows (results in the scratch `re-work\final-results.json`) and live runs
(`out\runs\step3-zoneid-ok`, `step3-loadmission`):

1. **Mission data reaches the zone.** Travel from the logged-in lobby with
   `/Game/Maps/Zones/Zone_Temperate_Forest?game=/Script/FortniteGame.FortGameMode?zoneid=<zone instance JSON>`
   (the same JSON the client's matchmaking sends). AFortGameMode::InitGame puts it in CurrentZoneInstanceId; the
   world manager then GETs world/info and POSTs world/validate, and the world record gets the theater, tile 11,
   ZT_TheForest and MissionGen_1Gate. It only worked after `fixes.cpp`: the world manager sets an empty user on
   the UFortMcpWorlds singleton (GetMcpWorlds 007CA5C0), which makes the requests need 'DedicatedServer' service
   permissions ("Unable to find service permissions for server."); the hook keeps the logged-in player instead.
   Coming from the logged-in lobby, the client sends its own ready RPCs and the match starts by itself.
2. **Nothing native generates missions** (AFortMissionGenerationManager is an empty shell; MissionGenerator only
   feeds the lobby loading screen), but `UFortMissionLibrary::LoadMission(WorldContext, MissionInfo)` works:
   `load /Game/Blueprints/Adventures/Gate/GateWithBluGlo.GateWithBluGlo` then
   `call /Script/FortniteGame.Default__FortMissionLibrary /Script/FortniteGame.FortMissionLibrary.LoadMission o:gm o:/Game/Blueprints/Adventures/Gate/GateWithBluGlo.GateWithBluGlo`
   spawns Mission_Gates_C with its 10 objectives, registers it (Missions: 1), activates and starts it.
3. **It then destroys itself**, as predicted: its BlueprintOnActivated calls FortMission.SpawnAtPlacementActorsAsArray,
   whose body is stubbed in this client (impl 00785E10), so the BP takes its failure branch
   ("Primary Mission Mission_Gates_C_0 set to SilentDestroy"). The zone does have the spots: of the first 60 of
   293 placement actors, 7 GatePlacementActor_C, 6 GateComplexPlacementActor_C, 4 GateSimplePlacementActor_C.
4. Remaining blockers being analysed: re-implementing SpawnAtPlacementActorsAsArray (spawn BP_Atlas_C at a gate
   placement actor, set its OriginatingPlacementActor, report success), the AI director (every getter returns
   null: stub 00EECAF0), and the Zone game state/controller classes the mission's AI encounters need.

## Step 4 (2026-10-26: Close the Gate playable, husks included): play the mission

Three more verified analysis workflows (results in the scratch `re-work\final2\*.txt`: atlas 20/22 claims held,
AI director 23/23, Zone classes 18/20, loading screen 18/19; nothing they rely on was refuted) found what the zone
still lacks. Static analysis used `tools\re.js`; VAs below are file VAs (image base 0x400000; the exe loads at
another base, so the DLL rebases them with `engine::Va`).

- **The loading screen waits for the Zone game state.** The screen drops only when the UI manager
  (GameInstance+0x4C) leaves state Invalid, and its only way out in a zone is the InGame trigger, which requires the
  GameState to be AFortGameStateZone. Setting the classes on Default__FortGameMode does nothing: AFortGameMode's
  constructor writes the plain Fort classes into every instance. The Zone classes must go into the instance before
  AGameMode::PreInitializeComponents (016BE5F0) spawns the game state. The Zone controller's OnRep of
  bHasServerFinishedLoading also switches to game-only input, so the player can actually play.
- **The AI director is real code.** Only its creation and its getters were compiled out: three getters were folded
  into the shared `xor eax,eax; ret` at 00EECAF0. Spawning AIDirector_Fortnite_C and AIGoalManager_C and pointing
  the 34 gameplay call sites at them restores it. It also needs the Zone game state (WorldReady comes from the world
  manager only with it) and a navmesh. This client never releases the initial navigation lock
  (bInitialBuildingLocked), and Zone_Temperate_Forest's persistent level has no navigation bounds. Whether husks
  can spawn is the open question.
- **More server code is stubbed:**
  - FortObjectiveBase ActivateObjective, StartPlayingObjective and StopObjective (vtable 02754320 slots 177/178/190).
  - K2_RemoveItemFromPlayer, used by the BluGlo deposit.
  - SpawnAtPlacementActorsAsArray.
  - All of the player's zone setup: nothing spawns the quickbars; K2_GiveItemToPlayer and
    ServerExecuteInventoryItem are stubs, so nothing ever equips.
  - The end of the zone: the MissionManager slot that would complete it is `ret 4`.

What `gameserver.dll` now does (`game.h`; each piece can be switched with `fix <name> on|off`):

| piece | how |
|---|---|
| zone | FortGameMode vtable slot 120 (02704CE0): writes FortGameStateZone/FortPlayerControllerZone/FortPlayerStateZone into a game mode opened with `?zoneid`, then calls 016BE5F0 (`game.cpp`) |
| player | on K2_OnRestartPlayer: world inventory and quickbars (spawned if missing), pickaxe, AR, wall/floor/stairs/roof, edit tool, ammo and materials (CreateTemporaryItemInstanceBP + QuickBars ServerAddItemInternal), equips the pickaxe, grants jump/sprint/interact through GE_GrantReflectMeleeDmg (Alpharium's trick) if the pawn has no abilities, fixes zero stats; equips on ServerExecuteInventoryItem; adds B_Pickups items on OnAboutToEnterBackpack; leaves bHasInitializedWorldInventory false (it would start MCP profile saves) (`player.cpp`) |
| atlas | 5-byte jump at 00785E10: finds the placement actors with the mission entry's tags, spawns each prop like SpawnBuildingActorAtTransform (deferred spawn, ISpawnable registration with the mission, finish, post-init) and marks the placement actor occupied (`atlas.cpp`) |
| objectives | the three objective vtable entries (status, OnRep, BP event, the mission's HandleObjectiveStatusChanged); SetMissionStatus logged; the call in K2_RemoveItemFromPlayer's exec (00A41042) removes the items (`objectives.cpp`) |
| director | GetAIDirector's UFunction wrapped; the 34 call sites redirected on the first game-thread tick; at InProgress spawns the goal manager and the director; FortGlobals.bEnableNewEncounters = false so Mission_Gates starts BaseGateEncounter_C; sizes the navigation bounds and rebuilds the navmesh; restores FortAIController.Possess and gives each husk its goal (`director.cpp`) |
| endzone | the mission manager's zone-complete slot: results screen, 30 s countdown, back to the lobby (`endzone.cpp`); `results off` keeps only the countdown and the return |
| autostart | LoadMission(GateWithBluGlo) once the match is InProgress, the world manager has been Running for 3 s, the player is set up and the director got WorldReady (or 15 s passed) |
| ui | if the HUD is not up 10 s after the player setup: FortAsyncAction_SetUIState(InGame) and SetInputMode_GameOnly |
| cheats | 100 BluGlo, free building, infinite ammo (for testing) |

To test: `Play OT6.5 (no Fiddler).bat`, log in, wait in the lobby, run `out\injector.exe`, then write to
`out\command.txt`:
`travel /Game/Maps/Zones/Zone_Temperate_Forest?game=/Script/FortniteGame.FortGameMode?zoneid={"worldId":"","theaterId":"33A2311D4AE64B361CCE27BC9F313C8B","theaterMissionId":"240487D1CE39E94760999521942A2B37","zoneThemeClass":"/Game/World/ZoneThemes/ZT_TheForest/BP_ZT_TheForest.BP_ZT_TheForest_C"}`.
Everything after that is automatic. Hold E on the Atlas to place it, deposit the BluGlo and start the defense. The
commands also work from the game's console (`) as `cheatscript <command>`; `husks` lists the husks and their goals.

Not done yet: rewards (nothing reaches the profile after a win), the lobby's Start Mission button (still "No Matches
Available"), other missions, co-op.

### First complete mission (2026-10-26, `out\runs\step4-first-run`, `step4-mission-complete`)

- **Run 1:** the Zone classes fixed the loading screen. The HUD, kit, director (WorldReady), LoadMission, the Atlas on a
  real power source and the objectives all worked.
  - New blocker: the pawn spawns in stasis (AFortPlayerPawn.StasisMode +0x870 = 1, "WAITING TO START...").
    Setting 0 plus FortPlayerPawn.HandleInStasis releases it.
  - Crash: the pickup handler read AFortPickup offsets from the B_Pickups_C effect, an AFortPickupEffect with its
    ItemDefinition at +0x248. Fixed, and engine::ProcessEvent now refuses objects that are not live.
- **Run 2:** releasing stasis 0.2 s after the spawn dropped the pawn through ground that had not streamed in yet.
  - Respawns land around (0,0) and fall. A session watcher teleported each new pawn above the Atlas with
    Actor.K2_TeleportTo.
  - The pawn had 2 abilities, so jump/sprint/interact were missing until `abilities` forced the grant.
  - The interact key never targets BP_Atlas_C (BlueprintCanInteract is never asked). A direct
    BP_Atlas_C.BlueprintOnInteract(pawn) ran PlaceAtlas, the BluGlo deposit and the activation (it toggles start and
    cancel).
  - The encounter (BaseGateEncounter_C) was created but stays in InitializingRiftManager, so no husks spawned. `nav`
    found an existing FortNavMesh-Husk and released the lock.
  - The defense completed on the Atlas's charge timer, and Mission_Gates_C reached **Succeeded**. Nothing ends the zone
    afterwards.
- All of these are fixed in the next section.

### Husks and the end of the zone (2026-10-26 evening, `out\runs\step4-husks` and later runs)

- **Spawn and stasis:** the zone layout has no APlayerStart, so the ChoosePlayerStart entry (02704DC0, 016AF990)
  falls back to a random PlayerSpawnPlacementActor_C. A pawn below Z -5000 is put back there. Stasis is released once
  per pawn after InProgress, as soon as it stands (or after 1.5 s).
- **Interaction:** no team was ever created, so the player was a Spectator, and mission props only answer Team
  HumanCampaign. PreInitializeComponents now calls CreateTeam (006EA510) and the player joins it. The Atlas is then a
  normal hold-E interaction.
- **Hero:** the pawn is dressed from the player state's hero (Class_Commando if none). The weapon effect overlays are
  re-mastered to the body every tick, which ends the GetBoneMatrix spam.
- **Console:** the DLL creates a FortConsole and a FortCheatManager. FortCheatManager.CheatScript runs any command.txt
  command.
- **Navmesh:** the zone's only bounds are UnitNavMeshBounds, 1 uu at the origin. It is scaled by (28672, 28672, 8192),
  then UNavigationSystem::RebuildAll (0153A8C0) runs. About 1800 tiles build in the background in ~3 s. After that the
  rifts pass their EQS queries and the encounter spawns husks.
- **Husks, possession:** AFortAIController::Possess is `ret 4` (vtable entry 02679CF4). It now points at
  AAIController::Possess (013A2EF0), followed by RunBehaviorTree with the pawn's BehaviorTree (+0x8E4).
  - Possess calls the controller's own SetPawn override (005ADA00), which sets MyFortPawn.
  - UseBlackboard calls the InitializeBlackboard override (005A5950), which caches the blackboard key ids.
  - BeginPlay grants the ability sets under authority (0081DE60), so each husk has 16 abilities.
- **Husks, goals:** BT_HuskBase sleeps until the blackboard has a GoalActor. The goal manager only updates AIs listed in
  its map at +0x268, which its Tick (005D9490) walks, and nothing in the client ever adds to that map.
  - The DLL picks the goals instead, every 0.5 s (`director::UpdateGoals`): the player within 15 m (kept until 25 m),
    otherwise the mission's first prop, BP_Atlas_C.
  - It sets them with FortAIController.SetGoalActor(goal, bLocationAlwaysKnown = true) (005AD5C0). That writes
    GoalActor, GoalActorAlwaysPerceived and GoalLocationIsAlwaysKnown, so the tree skips hunting and goes to
    BT_MoveToWithUndermining and then the melee abilities.
  - Tested: husks walk to the Atlas at about 3 m/s, hit it, and attack the player.
- **End of the zone:** the mission manager slot 194 (027428DC) is `ret 4`. It is now hooked:
  1. 3 s after the primary mission finishes, the DLL writes the reward report and sets GameplayState 2, which shows the
     results (Victory or Defeat from LocalPlayer.CachedRewardReport).
  2. VoteTimeLeft counts down 30 s. SetGameplayState resets it, so the DLL writes it after the switch.
  3. Leave or timeout: GameplayState 4, then ClientStayWithParty and ClientReturnToMainMenu.
  4. If the zone is still loaded after 6 s, ClientTravel to FortniteEntry. If the lobby UI is not in FrontEnd after
     10 s, it is switched.

### Hero abilities, stats, traps and editing (2026-10-26 night)

- **Gadgets** (the Commando's frag grenade on X did nothing): ClientExecuteInventoryItem asks the item's definition to
  run it locally. UFortGadgetItemDefinition's slot (vtable +0x16C, 00758B80) activates the ability spec whose handle
  is in the item's entry (FFortItemEntry.GameplayAbilitySpecHandle, entry +0x78). The server code that grants gadget
  abilities and fills in that handle is compiled out, so the handle stayed 0 and TryActivateAbility found nothing.
  - `player::LinkGadget` finds the granted spec in ActivatableAbilities.Items (ASC +0x220; 0x50 each: Handle +0xC,
    Ability +0x10, SourceObject +0x1C). It writes the handle into the item and its replicated entry, and sets the spec's
    source to the gadget definition (what GetCurrentSourceItemDefinition reads).
  - The grenade's costs are 45 stamina and 1 AmmoDataFragGrenades, looked up in the controller's inventory.
- **Backpack:** the hero's kit items (gadgets, the grenades they throw) were saved too, and came back next to the
  hero's own: two of each. They are no longer saved, and are skipped in old saves.
- **Hero stats:** what the server ran is UFortGlobals::InitializePlayerGameplayAbilities (006FCB60). The lobby still
  runs it on an AFortPlayerAttributesProxyActor for the hero card. The zone now does its stat part (`loadout::ApplyStats`):
  1. UFortHero::ApplySharedGE (00749DA0) on the lobby's hero item: GE_SharedStats with Vitality/Offense/Prowess/
     DefenceMagnitude from the specialization's curves at the hero's level (HeroStatsGrowth: a level 20 Commando has
     Fortitude 50, Offense 83, Tech 54, Resistance 63). Without the item, the same curves (FCurveTableRowHandle::Eval,
     0166EFE0) go straight into the attributes.
  2. The hero type's CombinedStatGEs (GE_Map_*): MaxHealth and HealthRegenRate scale with Vitality, Shield and
     ShieldRegenRate with Defense, plus weapon and trap damage.
  3. The homebase bonuses: the record found by the local player (0079F8B0, a static map, so the lobby's survives the
     trip) applies its node effects (007B3330).
  4. Health and shield are topped up. The log shows the stats before, after the hero, and after the homebase.
  - First test (Ninja) broke the character, with health 0, no weapons, building or inventory. The game's own log
    (`%LOCALAPPDATA%\FortniteGame\Saved\Logs\FortniteGame.log`, worth reading) said every GE_Map modifier "was asked to
    CalculateMagnitude and failed, falling back to 0". Two causes:
    - The mapped effects capture Vitality/Defense from their source. ASC::MakeEffectContext (01F696E0) names the player
      state as instigator, and no ability system is found through it. The context is now made the same way, then
      AddInstigator (context vtable +8) names the pawn. As a safety net, the mapped effects are removed again if max
      health still ends below 1.
    - The bases were never set: the zone's sets start at health -1 and speeds 0. The server copied the pawn's stats row
      in (AFortPawn::GetPawnStats 0083C250, PawnStatHandle +0x530). FFortPlayerAttributeSets init (008ADDB0) on
      AFortPlayerState.AttributeSets (+0x5F0) writes them as bases through the owning ability system. It now runs first,
      from the hero's pawn class. Without a row, GetPawnStats returns a stand-in with speeds of 10, so then only the max
      health base is set.
  - The zone always spawned PlayerPawn_Generic_C. FortGameMode keeps AGameMode's GetDefaultPawnClassForController
    (vtable entry 02704DD4); it is wrapped to return the lobby hero's OverridePawnClass. The hero pawns are children of
    PlayerPawn_Generic_C and carry the class tags. Without them the quickbar refused the Ninja's gadgets ("does not
    have the RequiredEquipTags").
  - The ability system is the player state's, so it outlives the pawn. Kits and stats are now granted once per
    ability system, not again on each respawn.
- **Traps** ("Cannot find spot to place on!"): AFortDecoHelper.CanPlaceState NoLocation with attachment type None,
  because AFortDecoTool.ItemDefinition (+0x5F8) was never set. ServerSpawnDeco (008FF1F0) is real code but returns at
  once without it. Equipping a deco item now sets it and calls OnRep_ItemDefinition.
- **Editing:** the client asks ServerBeginEditingBuildingActor (real code, 00871780) only when
  ABuildingSMActor::CanBeEdited (vtable +0x47C, 00655D70) agrees. In no run was it ever asked. The conditions are an
  EditModeSupport (+0x6EC, made from EditModePatternData +0x6C0), bIsPlayerBuildable, no build animation (+0x6FA),
  nobody editing (+0x7A8), nothing blocking (+0x808), and bit 6 of +0x89C clear. The server also needs the edit tool
  from UFortGameData +0x994 in the inventory. `editcheck` logs all of these for the pieces near the player.
- **Guns and buildings:** by design. RangedWeapons gives EnvDmgPB 0 to 271 of its 275 rows; only test and explosive
  items (proximity mine, troll launcher) damage the environment.

### Launch screen, traps' team, editing, falls and patrols (2026-10-27 night)

- **Launch screen** (`lobby.cpp`): with a server, matchmaking ended in its pre-game lobby (LobbyBeacon). Now the Play
  tab's mission opens a local one instead:
  - FortMatchmakingContext's delegates are broadcast from here: OnMatchmakingStarted, OnMatchmakingComplete(Success),
    OnLobbyStarted, OnWorldRecordLoadedChanged(true), then OnLobbyTimeUpdated(seconds) every second. Each bound
    function is called by name (FScriptDelegate: a weak object, then an FName).
  - JoinFromLobby, ReadyUp, CancelMatchmaking, DisconnectFromLobby, IsInLobby and IsWorldRecordLoaded are answered
    here while it is up.
  - Select your Hero calls FortMcpProfileAccount.SetDefaultHero. Select Your Gadgets calls SetModeLoadout. The zone
    reads both through the backend's loadout route, and the picked gadgets go to primary slots 7 and 8 (keys 5 and 6,
    HotbarKey8/9).
- **Traps' team:** a placed piece takes the team of the player state whose WorldPlayerId it is given (00637510). That
  id stayed -1 (set in AFortPlayerState's constructor), so traps kept Team Spectator. Damage traps hit the player, and
  jump pads (TriggerFilter excludes enemies) ignored them. The player's PlayerID now stands in for it.
- **Editing:** G calls AFortPlayerController::PerformBuildingEditInteraction (vtable +0x9B4), then 0085BD50(3): the
  targeted piece's interaction check, then ServerAttemptInteract with the edit interaction. CanBeEdited (vtable
  +0x47C in BuildingSMActor/Floor/Stairs/Roof/Wall) is wrapped (`gameplay.cpp`): the build animation no longer
  blocks it, and any other refusal is logged with its parts.
- **Fall damage:** AFortPawn's landing code (0081FFF0) computes it under authority, from the pawn stats'
  FallingDamage curve times max health. It then calls 00720C70 (target, damage, tags, template GE), which is only a
  destructor here. ApplyGlobalEnvironmentDamageToActor's exec calls it too. Both calls now apply the template
  (GET_FallingDamage): the damage goes into its OutgoingBaseDamage override for the one application, with the pawn as
  the context's instigator.
- **Patrols:** the AI director's roaming husk groups (EDO_Set_Patrol, OnStartSpawningPATrolls in its blueprint) are
  started with FortCheatManager.ToggleSpawnPATrolls's toggle (005B1E70) once the mission loads (after the navmesh).
  The Atlas now pulls only husks within 150 m; a patrol farther off waits until the player comes within 25 m.

### Husks on the map, editing finished pieces, spawnhusk (2026-10-27, later)

- **No husks until the Atlas:** in a live game (DeliverGoods on Suburban, patrols on) `state` listed no AI pawns.
  The director's WorldReady handler (005A7E80) only spawns its ambient husks when the game is started with
  `-guardians` (SpawnGuardians) or `-patrolls`. The patrol toggle alone spawned nothing in two minutes.
- **Chest guardians** (`director::StartAmbient`): SpawnGuardians (005AF180) gathers the BuildingContainers whose byte
  at +0x974 is set. The tiered chests have it: Tiered_Chest_3_Child and Tiered_Chest_4_Child read 1. It passes them to
  OnReadyToSpawnGuardians, where AIDirector_Fortnite's Guardians graph places husks by each chest with
  SpawnAIFromClass, in the same call. That graph covers husks, pitchers, beehives, bombshells, dwarfs, huskies,
  sploders and blasters.
  - It now runs once per director, at mission load with the patrols.
  - The husks it places are marked as guardians. They wait by their chest and chase only a player within 15 m
    (giving up at 25 m), and the Atlas never pulls them.
- **Editing:** the only refusal left for the player's own pieces was bit 6 of +0x89C (bUnderConstruction). It is
  set while a piece goes up and cleared by server code that is compiled out, so no placed piece could ever be
  edited. The CanBeEdited wrapper now hides it as well as the build animation.
- **`spawnhusk <type> [count]`** (or `cheatscript spawnhusk ...`): spawns 1 to 10 husks in a row 6 m in front of the
  player. Each is spawned with SpawnActor (moved out of what it overlaps), then Pawn.SpawnDefaultController, so it
  is possessed and its behavior tree starts like any other husk's. With no type it lists the types:
  - husk, fire, ice, lightning, pitcher
  - husky (+ _fire/_ice/_lightning), dwarf (+ _fire/_ice/_lightning)
  - beehive, bombshell, poison, sploder, trashman, blaster, flinger
  - smasher (+ _fire/_ice/_lightning/_boss), taker, troll, werewolf

### Husk groups out on the map; every husk kind checked (2026-10-27)

- **The director's patrols never come.** StartAmbient switches the director's bSpawnPATrolls on and
  OnStartSpawningPATrolls fires (also by hand), but its blueprint never places a husk: a zone had only the chests'
  guardians (5-8) and, later, the Atlas's waves.
- **Groups placed by the DLL instead** (`director::UpdateRoamers`, switch `fix roamers on|off`): from 30 s after
  the mission loads until the zone ends, a group every 20 s while fewer than 8 + 4 per player (24 at most) are alive.
  A spot 4000-7000 from a random player, put on the navmesh (NavigationSystem.ProjectPointToNavigation, extent
  600/600/4000; GetRandomReachablePointInRadius returned the same spot every time), 3500 from every player; each
  member on the navmesh around it too (on a slope a point beside it was inside the ground and the husk fell out of
  the world). A group waits where it is until a player comes within 2500, then hunts them until 4000 (never pulled to
  the Atlas); groups every player left 14000 behind for a minute are taken away. Mixes: plain husks with pitchers;
  husks with huskies, dwarfs, pitchers and beehives; sploders, bombshells (and poison ones) and trashmen; fire, ice
  or lightning groups; now and then a smasher or blaster. (Flingers and takers were in the mix at first: the
  flinger's BT_Flinger circles its goal-manager assignment and throws the husks around it, neither of which a
  lone roaming flinger has, so it stood idle beside the player; the taker wasn't seen to attack. Both are out.)
- **Every kind in /Game/Characters/Enemies checked** with `spawnhusk` (all 28 spawn, are possessed and run their
  behavior tree; all but two chase and fight): the troll (Thief) and the werewolf (Transylvania) come with no
  abilities in this build, so they can't attack and aren't used. The boss smasher works but isn't in the mixes.
- Dying shows "YOU HAVE DIED! Click to respawn!"; a click (or `pc ServerRestartPlayer`) brings the player back.

### The Play tab's missions (2026-10-27)

- **world_info is from a later build:** 85 of its 150 missions named generators this client doesn't have (Plankerton's
  T2 missions, Canny Valley's and Twine's rocket launches); Play on those did nothing. The backend now serves each as
  the nearest generator the client has (src/routes/misc.js, logged at startup): Retrieve the Data -> RetrieveTheData,
  Ride the Lightning and the launches -> LaunchTheRocket, Deliver the Bomb -> DeliverTheGoods, Repair the Shelter and
  Build the Radar Grid -> PowerTheStormShield, the gates -> 2/3/4Gates, the rest (Destroy the Encampments, Evacuate the
  Shelter) -> 1Gate. Every tile opens a mission now (the outpost and onboarding tiles too, below).
- **The client's generators and their missions** (each loads, spawns its props through the placement hook and starts
  its objectives; tried with `tools/mission-run.sh`):
  - 1Gate/2Gates/3Gates/4Gates: Mission_Gates (locate, place the Atlas, BluGlo, activate, defend, bonuses). Played.
  - DeliverTheGoods: played on Suburban.
  - LaunchTheRocket: StormShield (BP_StormShield, the rocket; Missle_Console_C): reach it, deposit BluGlo at the
    console, activate, three defense phases, launch. Reaching the rocket starts the deposit.
  - RetrieveTheData: DeadDrop: the weather balloon lands after its timer (about 16 minutes on the clock) at the orange
    beam; locate, defend it, victory.
  - PowerTheStormShield: ScoutAndSupply: find the construction site (reaching it starts the next step), find the
    cloaked missing parts around the map (each uncloaks when a player walks into it) and bring them, complete the core
    install, activate the reactor control, defend the raid, the vantage point.
  - Husks attack the mission's first prop in each (the Atlas, the rocket, the balloon, the site).
  - Interactions are the game's own (E, hold); a scripted ServerAttemptInteract is refused by the props' own
    BlueprintCanInteract until the player has done what they ask, so these missions are for playing by hand.

### The tutorial ("help Ramirez") and the outposts (2026-10-27)

- **The maps are in this build:** Zones/TheFarmstead/Zone_Onboarding_FarmsteadFort (the tutorial, with its cutscene
  levels GameStart_Cine and Onboarding_Intro_Cine) and Zones/Outpost/Zone_Outpost_PvE_01..04 (the storm shield bases).
  The lobby's theme-to-map table has BP_ZT_Onboarding_Fort and BP_ZT_TheOutpost_PvE_01..04 now, so their Play tab
  tiles open (the tutorial is the one-tile "Stonewood" Tutorial theater).
- **The tutorial** (MissionGen_Onboarding_Fort -> OB_FarmsteadFort, Mission_FarmsteadFort): the intro cutscenes play
  (the Fortnite logo, Ray in mission control, Ray's camera on the hero), then the player is in the cave; breaking the
  boards gives wood and the tutorial activates its 23 steps (harvest for a melee weapon, craft the raptor, melee and
  gun, break the door, build, find the fort, traps, to mission control, the rocket, the balcony, defend the fort, the
  air strike, the siphon, build the fort, explore the caves, harvest ore, twine, metal, nuts and bolts). It is
  single-player and starts from nothing (`game::Tutorial()`): the hero's harvest tool, building pieces and the edit
  tool only (no kit, backpack, profile materials or cheats), nothing saved from it (the real backpack stays), no
  hosting, no roaming husks or chest guardians. Only its first steps were tried by script; the rest is for playing.
- **The outposts:** FortMatchmakingContext.EnterOutpost (the way in from the main menu) asked for an outpost server
  and IsOutpostPermitted could grey it out; both are wrapped like the Play button: Enter Outpost opens the current
  theater's outpost (Stonewood 1, Plankerton 2, Canny Valley 3, Twine Peaks 4) with its own mission
  (MissionInfo_Outpost_PvE_0N: Mission_OutpostSetup, the storm shield's setup and defenses), and the outpost is always
  permitted. The world manager has the backend validate the zone's mission or the zone never loads ("LoadZoneWorld
  Failed"), so a mission of that theater goes along. Console: `outpost [1-4]`. What is built there isn't kept
  between visits (the outpost's world record isn't saved).
- Tests: `tools/mission-run.sh Onboarding_Fort Onboarding`, `tools/mission-run.sh TheOutpost_PvE_01 Outpost1`.

## Co-op (2026-10-27): up to 4 players in one zone (`net.cpp`)

One copy of the game hosts the zone (it is the server, as the host's own player always was), the others join it as
plain clients. Tested with two copies on one PC (tools/coop-run.sh): both in the same zone, moving and seeing each
other, the joined player set up like the host's own (hero, stats, inventory, quickbars), the end of the zone, rewards
and the trip back to the lobby for both.

### Playing

- **Starting a mission together:** one player picks a mission on the Play tab and presses Play. Their launch screen
  (Select your Hero, the 60 s clock) is open to the others: whoever presses Play while it is up lands on the same
  launch screen, same clock, that player's mission (whatever they had selected). When the clock runs out or anyone
  presses Launch, the first player opens the zone and the others join it as soon as it listens (they wait up to 3
  minutes). At most 4 players.
- **Joining a game already running:** press Play while another player is in a zone: the launch screen hands over to
  joining it.
- The host's zone listens on UDP port 7777 once its mission is loaded (`fix host off` stops that) and is announced to
  the backend then (`ot65/session`, refreshed every 15 s, removed when the host leaves).
- Backend (Fortify-Alpha src/routes/ot65.js): `POST ot65/lobby/play` (Play pressed: joins an open launch screen,
  else answers a running game's address, else opens this player's), `GET ot65/lobby/<host>` (the clock, launched,
  players, the host's zone address once announced; asked every second), `POST ot65/lobby/<host>/launch`,
  `POST ot65/lobby/<host>/leave`. They aren't local-only (a friend's game on another PC uses them) and only touch the
  in-memory table of launch screens.
- Console: `cheatscript join` (the first hosted game), `join <ip[:port]>`, `host [port]`, `netstat`.
- Test helpers: `lobbyplay [generator part]` (Play on the Play tab, as a click; with the Play tab never opened,
  Close the Gate's launch screen), `playmission <generator> [zone]` (that mission at once), `goto <class | actor name |
  goal>` (teleport beside it, on the navmesh), `props <object> [name part]` and `params <function>` (UPROPERTYs and
  parameters with offsets and values), `bring` (host: joined players next to the host), `walk [s]`, `equip <item name part>`,
  `drop <item name part>` (all of it, as the inventory screen drops it), `give <item path> [count] [all]` (`all`: the
  joined players too), `face <class name part | actor name>` (turn to the nearest; with `walk` and `pc StartFire` a
  test swing), `actors <class name part> near` (the 20 nearest live ones), `netactor <name part>` (why an actor is or
  isn't sent to each player), `netdestroyed <name part>` (destroyed placed actors on record, sent to whom),
  `levels` (streaming levels), `netdormancy on|off`.
- Each copy of the game logs to its own folder: out\gameserver.log, then out\instance2\ and so on.
- tools/coop-run.sh lobby / solo: both copies (or one) left in the main menu; tools/mission-run.sh <generator> [zone]:
  one copy straight into that mission, god mode on, its objectives listed.
- tools/coop-run.sh: one test round on this PC (closes the copies, starts a host and a joiner as coop1/coop2 with
  -AUTH_LOGIN, injects both, hosts Close the Gate, joins). tools/Coop-Test.ps1 starts one more copy.

### What the client lacked, and what replaces it

- **Listening.** UWorld::Listen is compiled out (00C79560 'xor al,al; ret 4'). `net::Host`: UEngine::
  CreateNamedNetDriver(world, GameNetDriver 282, GameNetDriver) (019802E0), FindNamedNetDriver in the world context
  (01984710), UWorld.NetDriver (+0x24) = driver, UNetDriver::SetWorld (017B4D70: registers its ticks, fills the network
  object list), IpNetDriver.InitListen (vtable +0xF8) with the world's FNetworkNotify (world+0x1C). The driver itself
  is made on the zone's first tick (`net::Prepare`) and only used from the mission's load: a world that listens while
  it generates never streams its tiles in, but a driver in the world context hears about every destroyed actor
  (below). A zone that never listened takes it out again (DestroyNamedNetDriver for a world context, 019816F0).
- **Replication.** UNetDriver::ServerReplicateActors is compiled out (TickFlush 017B6E70 still asks IsServer, then
  nothing). TickFlush (IpNetDriver vtable +0x11C) is wrapped and does the loop the replay recorder still does
  (TickDemoRecord 016A5C00, DemoReplicateActor 0168BEF0):
  - ReplicationFrame (driver+0x1DC) bumped every frame; without it an actor's properties are compared only once, so
    nothing but its first state ever went out.
  - The due actors of the network object list (driver+0x274, NextUpdateTime vs UWorld.TimeSeconds +0x4E0), longest
    waiting first; placed actors still DORM_Initial (actor+0xC9 == 4) skipped; CallPreReplication (014F0390).
  - Per connection: the player's own controller and pawn first; ClientHasInitializedLevelFor (conn vtable +0x144);
    AActor::IsNetRelevantFor (vtable +0x1D8); channel lookup in ActorChannels (+0x28F44, TSet FindId 01220450),
    CreateChannel (017A08D0) + SetChannelActor (01681D70); ReplicateActor (0167ED10) unless the channel's reliable
    buffer (+0x3C) or the connection (IsNetReady, vtable +0x11C) is full.
  - Moves: UCharacterMovementComponent::SendClientAdjustment (016065A0, through its interface at +0x100).
  - Net speed 200 KB/s per connection (UPlayer.CurrentNetSpeed +0x24; the client asks for 20000).
- **Joining.** AFortGameSession::ValidatePlayer (007B5770) wants a matchmaking reservation ("No reservation in game..
  kicking"): its two calls let players in while hosting (up to 3 joined).
- **The joined player's pawn.** FortGameModeZone (FortniteServer module, not in the client) restarted late joiners:
  AFortGameMode::RestartPlayer (vtable +0x3DC) once their loadout is known. The controller setup it reaches casts a
  null MCP profile (another account's profile query is a dedicated-server request): that call (00876FBC -> 00833C30)
  is skipped without one.
- **Their setup.** player::Setup runs for every player: the loadout is per account now (`loadout::Fetch/Get/Resolve`
  by the player state's net id), the hero's pawn class per controller, stats from the hero's curves (the host's own
  profile item and homebase record only for the host's player). The HUD refreshes are the local player's only.
- **Their inventory.** FFortItemList (inventory+0x244) is a fast array: every added, changed or removed entry gets its
  ReplicationID/ReplicationKey and the array key bumped (`MarkEntryDirty`/`MarkArrayDirty`), or a joined client sees
  an empty inventory.
- **Their loading screen.** It waits for the player state's bHasFinishedLoading (+0x2F0 bit 2), set by server code that
  is compiled out: set when their setup ran.
- **The zone's tiles on a client.** Each foundation replicates LevelToStream and builds its streaming level there, but
  the "load and show" (AGameMode::ReplicateStreamingStatus, vtable +0x378) went out once at join, before those levels
  existed on the client: sent again every 5 s for 3 minutes, then every 30 s. A joined client flushes level
  streaming (GameplayStatics.FlushLevelStreaming) every 2 s, as the server's world manager does.
- **Destroyed actors.** UNetDriver::NotifyActorDestroyed (IpNetDriver vtable +0x134, 017AD250) kept only its property
  tracker cleanup. Wrapped: the actor leaves the network object list (TSet::Remove 00CA7930), and every player's
  channel for it closes (the client destroys its copy then): hit effects, picked-up loot, harvested trees, broken
  buildings. A destroyed placed actor also goes on a record (the engine's DestroyedStartupOrDormantActors): its static
  NetGUID (FNetGUIDCache::GetOrAssignNetGUID 017A7CC0 on the driver's cache +0x70), level and name. Each frame every
  player without a channel for it gets the engine's destroy message once its level is loaded there
  (SetChannelActorForDestroy, stripped too): a new actor channel whose one reliable bunch opens and closes it and
  names the actor (FOutBunch 01667060, UPackageMapClient::WriteObject vtable +0xEC, SendBunch channel vtable +0x114),
  up to 64 a frame. A zone takes away about a thousand placed actors as it sets up (bushes, trees, mushrooms and
  other searchables around the spawn and the mission's pieces); without the record a joined player saw all of them.
  (Sending a dying actor through ReplicateActor instead writes its sub-objects, which the client can't resolve: it
  drops the connection.)
- **Dropping the held weapon** (both players): the drop takes the item away, and if the pawn holds that item's weapon
  (FortWeapon.ItemEntryGuid) the pickaxe comes out (`player::AfterDrop`); the dropped weapon stayed in the hands with
  its slot empty.
- **Husks** go for the nearest player (net::PlayerPawns).
- **The end of the zone.** Rewards are reported for every player's account; their backpacks are saved and they are sent
  home (ClientReturnToMainMenu) before the host leaves; a joined player's own leave request sends only them home. A
  joined copy writes its own results report from the replicated mission as soon as it ended (the screen picks
  Victory/Defeat when GameplayState 2 arrives) and fixes its lobby UI once back.

### Not tested yet

- By hand in the joined player's window: building and editing, harvesting, shooting husks, traps, gadgets and hero
  abilities, pickups, crafting. (Their server calls are the ones the host's own player uses; a joined player's
  ServerCreateBuildingActor and ServerExecuteInventoryItem were seen arriving.)
- A friend on another PC (set up below, not tried yet).

### Chat and presence (2026-10-27)

- The main menu's chat ("Unable to chat while offline") and the "Connecting..." corner wait for the game's XMPP
  service. The backend now runs one (Fortify-Alpha src/xmpp.js, TCP 5222, started by index.js): SASL PLAIN with the
  account id and the game's access token (checked with the backend's JWT secret), resource binding, sessions,
  pings, the roster (every other account on the backend is a friend, subscription both), presence to everyone
  online, chat rooms (the GLOBAL channel is fortnite@muc.prod.ol.epicgames.com; the party's own rooms the same way)
  and private messages. Unknown stanzas get an empty result; the first ones of each kind are logged as [XMPP].
- The patch pak points the client at it: [OnlineSubsystemMcp.XMPP] and [OnlineSubsystemMcp.XMPP Prod] ServerAddr =
  the backend's host, ServerPort=5222, bUseSSL=false (the section is read with the MCP config's name appended).
- libjingle refuses PLAIN on an unencrypted stream unless allow_plain is set (PlainSaslHandler::
  ChooseBestSaslMechanism, 024E86E0, "XMPP log-in credentials not valid (6)"): the DLL turns its jne at 024E86E8
  into a jmp, first thing when it loads. The game logs into chat right after its own login and doesn't retry, so the
  DLL has to be in by then: Play-OT65-Direct.ps1 now injects it as the game starts (the DLL waits for the engine
  tables by itself), and so does tools/coop-run.sh. Injecting by hand still works if it is done on the login screen,
  before logging in.
- Tested with two copies: both log in, join GLOBAL, see each other; a line said in GLOBAL shows in the game's chat.
  The bottom-right corner shows the player's party card instead of "Connecting...".

### Playing with a friend on another PC

One PC runs the backend (accounts, profiles, the list of games and launch screens); every player's game talks to
it. Any of them can host a zone: whoever presses Play first opens the launch screen and hosts, the others join. The
backend's ot65 routes answer this PC and private networks only (LAN, Radmin VPN 26.x, Tailscale 100.64-127.x), so a
friend's hosting copy can read and save its players' loadouts; a game hosted on another PC is announced at that PC's
own address. A player who hosts needs UDP 7777 allowed in their own firewall.

1. Put both PCs on one network. A free VPN is the simple way over the internet (Tailscale or Radmin VPN: both install
   it, join the same network, and the host notes their VPN address, e.g. 100.x.y.z). On the same LAN, the host's LAN
   address works. Forwarding ports on a router also works (TCP 3551 for the backend, TCP 5222 for chat, UDP 7777 for
   the game), but it exposes the backend, which has no real security.
2. The host allows FortniteClient-Win64-Shipping.exe and node through Windows Firewall when Windows asks (private
   networks).
3. The host makes the friend's patch pak: `node tools\build-patch-pak.js <host address>:3551 friend\FortniteGame-WindowsClient_p.pak`.
4. The friend needs the same OT6.5 build folder, the OTGameServer out folder (injector.exe, gameserver.dll), that patch
   pak in their FortniteGame\Content\Paks folder, and a backend.txt next to gameserver.dll with `<host address>:3551`.
5. The friend starts the game (FortniteClient-Win64-Shipping.exe, or FortniteLauncher.exe with the same arguments as
   tools\Play-OT65-Direct.ps1), logs in with any new name (the host's backend makes the account), runs injector.exe,
   and presses Play while the host is in a zone (or types `cheatscript join`).
