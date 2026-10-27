#include "game.h"
#include "engine.h"
#include "log.h"
#include "mission.h"
#include "trace.h"
#include <windows.h>
#include <algorithm>
#include <functional>
#include <vector>

namespace game {
Switches g_on;
}

namespace zone {

namespace {
// AGameMode::PreInitializeComponents (016BE5F0, FortGameMode vtable slot 120) spawns GameStateClass (+0x29C), and
// Login later spawns PlayerControllerClass (+0x28C) and PlayerStateClass (+0x298). AFortGameMode's constructor
// writes the plain Fort classes into every instance (the class default object never reaches it), so the vtable entry
// writes the Zone classes into the instance first, for a game mode opened with a zone instance id (?zoneid=...).
constexpr uint32_t kPreInitializeEntry = 0x02704CE0, kPreInitialize = 0x016BE5F0;
constexpr int GameModePlayerControllerClass = 0x28C, GameModePlayerStateClass = 0x298, GameModeGameStateClass = 0x29C;
constexpr int FortGameModeZoneInstanceId = 0x2F4;  // FString, parsed by InitGame before this runs
constexpr int GameModeGameState = 0x2A0, GameStateTeams = 0x374;  // TArray<AFortTeamInfo*>
// Nothing in the client creates teams (InitializeTeams has no caller), so login puts the player on Spectator and every
// mission prop (Team HumanCampaign) refuses interaction before its Blueprint is asked. AFortGameMode::CreateTeam
// spawns the team; the native AssignTeam at PostLogin then places the player on it.
constexpr uint32_t kCreateTeam = 0x006EA510;  // thiscall(gm, EFortTeam), ret 4; 0 = HumanCampaign
// AGameMode::ChoosePlayerStart (FortGameMode vtable slot 176) looks only at APlayerStart actors. Only 9 of the 19
// forest tiles have one, so some layouts have none: the pawn then spawns at the world origin over nothing and falls.
// Every tile has PlayerSpawnPlacementActor_C spots (the spawn data the absent server module used); one is the fallback.
constexpr uint32_t kChoosePlayerStartEntry = 0x02704DC0, kChoosePlayerStart = 0x016AF990;  // thiscall(gm, controller), ret 4
constexpr uint32_t kPlacementActorClass = 0x00934570, kPackageFortniteGame = 0x02668A10;
constexpr uint32_t kGetAllActorsOfClass = 0x016B4FB0;
constexpr int GameModeFailedStarts = 0x368;  // TArray<AActor*> of starts SpawnDefaultPawnFor could not use
uintptr_t g_gameState = 0, g_controller = 0, g_playerState = 0;
uintptr_t g_spawnWorld = 0, g_spawnSpot = 0;  // the fallback spot, chosen once per world

void __fastcall PreInitializeComponents(uintptr_t gameMode, void*) {
    bool zone = game::g_on.zone && engine::At<int32_t>(gameMode, FortGameModeZoneInstanceId + 4) > 1;
    if (zone) {
        engine::At<uint32_t>(gameMode, GameModeGameStateClass) = static_cast<uint32_t>(g_gameState);
        engine::At<uint32_t>(gameMode, GameModePlayerControllerClass) = static_cast<uint32_t>(g_controller);
        engine::At<uint32_t>(gameMode, GameModePlayerStateClass) = static_cast<uint32_t>(g_playerState);
        LOG("zone: %s spawns the Zone game state, player controller and player state", engine::Describe(gameMode).c_str());
    }
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(engine::Va(kPreInitialize))(gameMode);  // spawns the game state
    uintptr_t gameState = engine::At<uint32_t>(gameMode, GameModeGameState);
    if (zone && engine::IsA(gameState, "FortGameState")) {
        reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t)>(engine::Va(kCreateTeam))(gameMode, 0);
        LOG("zone: created the HumanCampaign team (%d team(s))", engine::At<int32_t>(gameState, GameStateTeams + 4));
    }
}

// SpawnDefaultPawnFor retries up to 5 times, listing the starts it could not spawn at in GM+0x368; a listed spot is
// skipped so a retry gets another one.
bool FailedStart(uintptr_t gameMode, uintptr_t actor) {
    uintptr_t failed = engine::At<uint32_t>(gameMode, GameModeFailedStarts);
    int32_t count = engine::At<int32_t>(gameMode, GameModeFailedStarts + 4);
    for (int32_t i = 0; failed && i < count && i < 16; i++)
        if (engine::At<uint32_t>(failed, i * 4) == actor) return true;
    return false;
}

uintptr_t FallbackSpawn(uintptr_t gameMode) {
    uintptr_t world = engine::World();
    if (world != g_spawnWorld) {
        g_spawnWorld = world;
        g_spawnSpot = 0;
    }
    if (g_spawnSpot && engine::IsLive(g_spawnSpot) && !FailedStart(gameMode, g_spawnSpot)) return g_spawnSpot;
    auto package = reinterpret_cast<const wchar_t*>(engine::Va(kPackageFortniteGame));
    uintptr_t placementClass = reinterpret_cast<uintptr_t(__cdecl*)(const wchar_t*)>(engine::Va(kPlacementActorClass))(package);
    engine::TArray all{};
    reinterpret_cast<void(__cdecl*)(uintptr_t, uintptr_t, engine::TArray*)>(engine::Va(kGetAllActorsOfClass))(
        gameMode, placementClass, &all);
    std::vector<uintptr_t> spots;
    for (int32_t i = 0; all.data && i < all.num; i++) {
        uintptr_t actor = engine::At<uint32_t>(all.data, i * 4);
        if (actor && engine::Name(engine::At<uint32_t>(actor, 0xC)) == "PlayerSpawnPlacementActor_C" && !FailedStart(gameMode, actor))
            spots.push_back(actor);
    }
    engine::Free(reinterpret_cast<void*>(all.data));
    if (spots.empty()) return 0;  // tiles not in yet (at Login): the engine falls back, OnWorldReady asks again
    g_spawnSpot = spots[GetTickCount() % spots.size()];
    float at[3] = {};
    engine::Location(g_spawnSpot, at);
    LOG("zone: no player start in this layout; spawning at %s (%.0f, %.0f, %.0f), one of %zu spawn spots",
        engine::Name(g_spawnSpot).c_str(), at[0], at[1], at[2], spots.size());
    return g_spawnSpot;
}

uintptr_t __fastcall ChoosePlayerStart(uintptr_t gameMode, void*, uintptr_t controller) {
    uintptr_t start = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kChoosePlayerStart))(
        gameMode, controller);
    return start || !game::g_on.zone ? start : FallbackSpawn(gameMode);
}
}  // namespace

uintptr_t SpawnSpot() {
    return g_spawnSpot && engine::World() == g_spawnWorld && engine::IsLive(g_spawnSpot) ? g_spawnSpot : 0;
}

bool Install() {
    g_gameState = engine::FindClass("/Script/FortniteGame.FortGameStateZone");
    g_controller = engine::FindClass("/Script/FortniteGame.FortPlayerControllerZone");
    g_playerState = engine::FindClass("/Script/FortniteGame.FortPlayerStateZone");
    if (!g_gameState || !g_controller || !g_playerState) {
        LOG("zone: the Zone classes were not found; left alone");
        return false;
    }
    bool ok = engine::PatchPointer(kPreInitializeEntry, kPreInitialize, reinterpret_cast<void*>(&PreInitializeComponents));
    bool spawn = engine::PatchPointer(kChoosePlayerStartEntry, kChoosePlayerStart, reinterpret_cast<void*>(&ChoosePlayerStart));
    LOG("zone: FortGameMode PreInitializeComponents %s, ChoosePlayerStart %s", ok ? "replaced" : "NOT replaced",
        spawn ? "replaced" : "NOT replaced");
    return ok;
}

}  // namespace zone

namespace game {

using engine::At;

namespace {
enum Event { kNone, kMatchState, kRestartPlayer, kHudEnterState, kPickup, kExecuteItem, kConsoleCommand, kWorldTick,
             kHudZoneCompleted, kCraftRequest, kDisassembleRequest, kSwapRequest, kEquipRequest, kBuildingDamaged,
             kLeaveZone };
const struct {
    const char* path;
    Event event;
} kEvents[] = {
    { "/Script/Engine.GameMode.K2_OnSetMatchState", kMatchState },
    { "/Script/Engine.GameMode.K2_OnRestartPlayer", kRestartPlayer },
    { "/Game/UI/Master_Layouts/NewHUD.NewHUD_C.OnEnterState", kHudEnterState },
    { "/Game/Effects/Fort_Effects/Gameplay/Pickups/B_Pickups.B_Pickups_C.OnAboutToEnterBackpack", kPickup },
    { "/Script/FortniteGame.FortPlayerController.ServerExecuteInventoryItem", kExecuteItem },
    // The console's way into the DLL: `cheatscript <command>` (its native body only looks up an ini section).
    { "/Script/FortniteGame.FortCheatManager.CheatScript", kConsoleCommand },
    // Actor ticks are world-tick time: the end of the zone changes the HUD and travels only from here.
    { "/Script/Engine.Actor.ReceiveTick", kWorldTick },
    { "/Game/UI/Master_Layouts/NewHUD.NewHUD_C.HandleZoneCompleted", kHudZoneCompleted },
    // The inventory screen's requests, logged to see which of them work.
    { "/Script/FortniteGame.FortPlayerController.ServerCraftSchematic", kCraftRequest },
    { "/Script/FortniteGame.FortPlayerController.ServerDisassembleInventoryItem", kDisassembleRequest },
    { "/Script/FortniteGame.FortQuickBars.ServerSwapItemsInternal", kSwapRequest },
    { "/Script/FortniteGame.FortPawn.ServerInternalEquipWeapon", kEquipRequest },
    // A building (tree, car, wall) took damage: its health bar learns of it only through the health set's OnRep.
    { "/Script/FortniteGame.BuildingActor.OnDamageServer", kBuildingDamaged },
    // The menu's Leave Zone (LeaveButton): the backpack is saved before the zone goes.
    { "/Script/FortniteGame.FortPlayerController.ServerReturnToMainMenu", kLeaveZone },
};

const struct {
    const char* name;
    bool* on;
} kSwitches[] = {
    { "zone", &g_on.zone }, { "player", &g_on.player }, { "atlas", &g_on.atlas }, { "objectives", &g_on.objectives },
    { "director", &g_on.director }, { "autostart", &g_on.autostart }, { "ui", &g_on.ui }, { "cheats", &g_on.cheats },
    { "endzone", &g_on.endzone }, { "results", &g_on.results }, { "inventory", &g_on.inventory },
    { "host", &g_on.host }, { "roamers", &g_on.roamers },
};

constexpr wchar_t kDefaultMission[] = L"/Game/Blueprints/Adventures/Gate/GateWithBluGlo.GateWithBluGlo";
// `play`: Close the Gate in Stonewood, the zone the lobby's Start Mission button would open if a server answered
// matchmaking.
constexpr char kPlayUrl[] =
    "/Game/Maps/Zones/Zone_Temperate_Forest?game=/Script/FortniteGame.FortGameMode?zoneid={\"worldId\":\"\","
    "\"theaterId\":\"33A2311D4AE64B361CCE27BC9F313C8B\",\"theaterMissionId\":\"240487D1CE39E94760999521942A2B37\","
    "\"zoneThemeClass\":\"/Game/World/ZoneThemes/ZT_TheForest/BP_ZT_TheForest.BP_ZT_TheForest_C\"}";
constexpr int GameStateWorldManager = 0x328, WorldManagerState = 0x268;  // state 5: Running
constexpr int GameStateGameplayState = 0x330;  // EFortGameplayState: 2 EndOfZone, 4 LeavingZone
constexpr ULONGLONG kTickMs = 200, kPawnWaitMs = 3000, kRunningWaitMs = 3000, kDirectorWaitMs = 15000, kUiWaitMs = 10000;
constexpr ULONGLONG kStasisWaitMs = 1500, kNavigationWaitMs = 120000, kNavigationIdleMs = 10000;
constexpr ULONGLONG kHeroWaitMs = 6000;  // the setup waits this long for the backend's answer about the lobby hero
constexpr int PawnCharacterMovement = 0x288, MovementMode = 0x120;  // EMovementMode: 1 Walking
constexpr float kFallenZ = -5000.0f;  // KillZ is -8192 in this zone
constexpr uint32_t kGEngine = 0x03267DD8;
constexpr int EngineConsoleClass = 0x90, EngineGameViewport = 0x42C, ViewportConsole = 0x24;
constexpr int PlayerControllerCheatManager = 0x2F0;

// What happened in the current world. Reset when GWorld changes.
struct Zone {
    uintptr_t world = 0;
    bool inProgress = false, restarted = false;
    uintptr_t setupPawn = 0, releasedPawn = 0;
    ULONGLONG pawnSince = 0, setupAt = 0, runningSince = 0, directorAt = 0, inProgressAt = 0;
    bool directorRequested = false, directorReadyLogged = false, missionLoaded = false, uiForced = false, hostTried = false,
         recordTried = false;
    // The navmesh: requested once the world manager runs; ready when its pending work, once seen, drains.
    bool navigationRequested = false, navigationBusy = false, navigationReady = false;
    ULONGLONG navigationAt = 0, navigationLogAt = 0;
};
Zone g_zone;
std::vector<std::function<void()>> g_queue;  // filled by commands under the lock, run by Tick without it

void LoadMission(const std::wstring& path) {
    uintptr_t info = engine::Load(path);
    uintptr_t library = engine::FindObject("/Script/FortniteGame.Default__FortMissionLibrary", "FortMissionLibrary");
    uintptr_t function = engine::FindFunction("/Script/FortniteGame.FortMissionLibrary.LoadMission");
    uintptr_t gameMode = engine::GameMode();
    if (!info || !library || !function || !gameMode) {
        LOG("game: can't load the mission %ls", path.c_str());
        return;
    }
    if (g_on.director) director::UseOldEncounters();
    LOG("game: LoadMission(%s)", engine::Describe(info).c_str());
    struct {
        uintptr_t context, info;
    } p{ gameMode, info };
    engine::ProcessEvent(library, function, &p);
    objectives::List();
}

// Damaged buildings, refreshed on the next world tick (their health is final by then). Game thread only.
std::vector<uintptr_t> g_damagedBuildings;
constexpr int BuildingAttributeSet = 0x2E0;  // ABuildingActor.BuildingAttributeSet, a UFortHealthSet

// On a client the health set's replication (OnRep_Health) tells the building's health bar; damage done here never
// replicates, so the bar stayed as it was when the building was first looked at.
void RefreshBuildingHealth() {
    std::vector<uintptr_t> buildings;
    buildings.swap(g_damagedBuildings);
    uintptr_t onRep = engine::FindFunction("/Script/FortniteGame.FortHealthSet.OnRep_Health");
    for (uintptr_t building : buildings) {
        uintptr_t set = engine::IsLive(building) ? engine::At<uint32_t>(building, BuildingAttributeSet) : 0;
        if (engine::IsA(set, "FortHealthSet") && onRep) engine::ProcessEvent(set, onRep, nullptr);
    }
}

bool IsZone(uintptr_t gameMode) {
    return gameMode && engine::Name(At<uint32_t>(gameMode, 0xC)) == "FortGameMode";
}

// Starts over when GWorld changes; called before events are recorded too, so none is lost to a late reset.
void SyncWorld() {
    uintptr_t world = engine::World();
    if (world == g_zone.world) return;
    g_zone = Zone{};
    g_zone.world = world;
    director::Reset();
    net::Reset();
    loadout::Reset();
    player::Reset();
    objectives::Reset();
    endzone::Reset();
}

// The game's console (` key) and a cheat manager, so `cheatscript <command>` reaches the DLL; the shipping client
// creates neither.
void EnsureConsole(uintptr_t pc) {
    uintptr_t gEngine = At<uint32_t>(engine::Va(kGEngine), 0);
    uintptr_t viewport = gEngine ? At<uint32_t>(gEngine, EngineGameViewport) : 0;
    if (viewport && !At<uint32_t>(viewport, ViewportConsole)) {
        uintptr_t console = engine::SpawnObject(At<uint32_t>(gEngine, EngineConsoleClass), viewport);
        At<uint32_t>(viewport, ViewportConsole) = static_cast<uint32_t>(console);
        LOG("game: created the console %s (open it with `)", engine::Describe(console).c_str());
    }
    if (pc && engine::IsLive(pc) && !At<uint32_t>(pc, PlayerControllerCheatManager)) {
        uintptr_t cls = engine::FindClass("/Script/FortniteGame.FortCheatManager");
        uintptr_t cheats = engine::SpawnObject(cls, pc);
        At<uint32_t>(pc, PlayerControllerCheatManager) = static_cast<uint32_t>(cheats);
        LOG("game: created the cheat manager %s", engine::Describe(cheats).c_str());
    }
}

// The navmesh: sized once the world manager runs (the tiles are in), then followed until its work drains.
void AdvanceNavigation(ULONGLONG now) {
    if (!g_zone.navigationRequested) {
        g_zone.navigationRequested = true;
        g_zone.navigationAt = now;
        if (!director::ExpandNavigation()) g_zone.navigationReady = true;  // nothing to wait for
        return;
    }
    if (g_zone.navigationReady) return;
    int pending = director::NavigationPending();
    if (pending > 0) g_zone.navigationBusy = true;
    if (pending > 0 && now - g_zone.navigationLogAt > 5000) {
        g_zone.navigationLogAt = now;
        LOG("game: navmesh building, %d item(s) pending", pending);
    }
    bool idle = !g_zone.navigationBusy && now - g_zone.navigationAt > kNavigationIdleMs;  // no work ever showed up
    if ((g_zone.navigationBusy && pending == 0) || idle || pending < 0 || now - g_zone.navigationAt > kNavigationWaitMs) {
        g_zone.navigationReady = true;
        LOG("game: navmesh %s after %llu s", pending == 0 ? "built" : "not finished, going on anyway",
            (now - g_zone.navigationAt) / 1000);
    }
}

void Advance(ULONGLONG now) {
    SyncWorld();
    director::Check();
    uintptr_t pc = engine::LocalPlayerController();
    EnsureConsole(pc);
    lobby::Advance(now);
    net::AdvanceClient(now);
    if (!IsZone(engine::GameMode())) return;
    uintptr_t pawn = engine::PawnOf(pc);

    // 0. Hosting: from the zone's first tick its net driver records the placed actors the zone takes away as it sets
    // up (bushes and trees cleared around the spawn and the mission's pieces), for joining players; it listens once the
    // mission is loaded (a world that listens while it generates doesn't).
    if (g_on.host && !g_zone.recordTried && !Tutorial()) {
        g_zone.recordTried = true;
        net::Prepare();
    }

    // 1. The player, once RestartPlayer has possessed the pawn (or the pawn has been there a while) and the backend has
    // said what the lobby has (hero, materials). During travel the local player can still point at the last world's
    // controller, hence the liveness checks.
    if (g_on.player && engine::IsLive(pc)) {
        loadout::Fetch(pc);
        loadout::Resolve(pc);
    }
    if (g_on.player && pawn && pawn != g_zone.setupPawn && engine::IsLive(pc) && engine::IsLive(pawn)) {
        if (!g_zone.pawnSince) g_zone.pawnSince = now;
        bool heroKnown = loadout::Settled(pc) || now - g_zone.pawnSince > kHeroWaitMs;
        if ((g_zone.restarted || now - g_zone.pawnSince > kPawnWaitMs) && heroKnown) {
            g_zone.setupPawn = pawn;
            g_zone.setupAt = now;
            g_zone.pawnSince = 0;
            player::Setup(pc, pawn);
        }
    }
    uintptr_t gameState = engine::GameState();
    uint8_t gameplayState = gameState ? At<uint8_t>(gameState, GameStateGameplayState) : 0;
    if (g_on.player && pawn && pawn == g_zone.setupPawn && engine::IsLive(pawn)) {
        // A pawn below the map (spawned over nothing) goes back to the spawn spot before anything else.
        float at[3], spot[3];
        uintptr_t spotActor = zone::SpawnSpot();
        if (engine::Location(pawn, at) && at[2] < kFallenZ && spotActor && engine::Location(spotActor, spot)) {
            spot[2] += 300.0f;
            engine::Teleport(pawn, spot);
            // A teleport keeps the fall's velocity; without this the landing can still kill.
            uintptr_t movement = At<uint32_t>(pawn, PawnCharacterMovement);
            if (movement) engine::ProcessEvent(movement, engine::FindFunction("/Script/Engine.MovementComponent.StopMovementImmediately"), nullptr);
            LOG("game: the pawn fell to Z %.0f; teleported it to the spawn spot", at[2]);
        }
        // The stasis players spawn in is released by the absent FortGameModeZone once the match starts; here once per
        // pawn, after InProgress and once it stands (or 1.5 s later). Not at the end of the zone or while leaving it.
        uintptr_t movement = At<uint32_t>(pawn, PawnCharacterMovement);
        bool standing = movement && At<uint8_t>(movement, MovementMode) == 1;
        if (g_zone.inProgress && pawn != g_zone.releasedPawn && gameplayState != 2 && gameplayState != 4 &&
            (standing || now - g_zone.inProgressAt > kStasisWaitMs)) {
            g_zone.releasedPawn = pawn;
            player::ReleaseStasis(pawn);
        }
        player::FixEffectMeshes(pawn);
        if (gameplayState != 2 && gameplayState != 4) loadout::AutoSave(pc, now);
    }

    // 2. The AI director, once the match runs; it gets WorldReady on the world manager's next Running tick.
    if (g_on.director && g_zone.inProgress && !g_zone.directorRequested) {
        g_zone.directorRequested = true;
        g_zone.directorAt = now;
        director::Spawn();
    }
    if (!g_zone.directorReadyLogged && director::Ready()) {
        g_zone.directorReadyLogged = true;
        LOG("game: the AI director got WorldReady; its encounter timer runs");
    }
    director::UpdateGoals();
    director::UpdateRoamers(g_zone.missionLoaded && gameplayState != 2 && gameplayState != 4 && !Tutorial());

    // 3. The mission, once the tiles are in (world manager Running), the navmesh is built, the player is set up and
    // the director is ready (or clearly won't be). A rift query made before the navmesh exists aborts the encounter.
    uintptr_t worldManager = gameState ? At<uint32_t>(gameState, GameStateWorldManager) : 0;
    bool running = worldManager && At<uint8_t>(worldManager, WorldManagerState) == 5;
    if (running && !g_zone.runningSince) g_zone.runningSince = now;
    if (g_on.director && g_zone.runningSince && now - g_zone.runningSince > kRunningWaitMs) AdvanceNavigation(now);
    bool directorSettled = !g_on.director || director::Ready() || (g_zone.directorAt && now - g_zone.directorAt > kDirectorWaitMs);
    bool navigationSettled = !g_on.director || g_zone.navigationReady;
    if (g_on.autostart && !g_zone.missionLoaded && g_zone.inProgress && g_zone.runningSince &&
        now - g_zone.runningSince > kRunningWaitMs && (g_zone.setupPawn || !g_on.player) && directorSettled &&
        navigationSettled) {
        g_zone.missionLoaded = true;
        if (g_on.director && !director::Ready()) LOG("game: starting the mission without a ready AI director");
        std::wstring chosen = lobby::MissionInfo();  // the Play tab's mission, else Close the Gate
        LoadMission(chosen.empty() ? std::wstring(kDefaultMission) : chosen);
        lobby::ForgetMission();
        // The husks outside the objective too, now that their spawn queries have a navmesh.
        if (g_on.director && director::Ready() && !Tutorial()) director::StartAmbient();
    }
    if (g_on.host && g_zone.missionLoaded && !g_zone.hostTried && !Tutorial()) {
        g_zone.hostTried = true;
        if (net::Host(0)) net::Open();
    }
    net::Advance(now);

    // 4. The HUD, if it still hasn't come up.
    if (g_on.ui && g_zone.setupAt && !g_zone.uiForced && now - g_zone.setupAt > kUiWaitMs && player::UiState() == 0) {
        g_zone.uiForced = true;
        LOG("game: the HUD is still not up; forcing the in-game UI");
        player::ForceInGameUI();
    }
}
}  // namespace

void Install() {
    zone::Install();
    atlas::Install();
    objectives::Install();
    director::Install();
    endzone::Install();
    inventory::Install();
    weapons::Install();
    lobby::Install();
    loadout::Install();
    gameplay::Install();
    net::Install();
}

int EventFor(const std::string& functionPath) {
    for (auto& e : kEvents)
        if (functionPath == e.path) return e.event;
    return kNone;
}

void OnEvent(int event, uintptr_t object, void* params) {
    if (event == kWorldTick) {  // dozens per frame: kept lock-free while there is nothing to do
        net::OnWorldTick();
        if (net::IsClient()) return;
        if (!g_damagedBuildings.empty()) RefreshBuildingHealth();
        endzone::OnWorldTick();
        return;
    }
    SyncWorld();
    if (net::IsClient()) return;  // joined to a host: its server runs the zone
    switch (event) {
    case kHudZoneCompleted:
        endzone::OnHudZoneCompleted();
        break;
    case kMatchState:
        if (params && !g_zone.inProgress && engine::NameOf(*static_cast<const int32_t*>(params)) == "InProgress") {
            g_zone.inProgress = true;
            g_zone.inProgressAt = GetTickCount64();
        }
        break;
    case kConsoleCommand:
        if (params) {
            const wchar_t* text = *static_cast<const wchar_t* const*>(params);
            int32_t length = static_cast<const int32_t*>(params)[1];
            std::string line;
            for (int32_t i = 0; text && i < length - 1 && i < 1000; i++) line.push_back(text[i] < 0x80 ? static_cast<char>(text[i]) : '?');
            if (!line.empty()) trace::Execute(line);
        }
        break;
    case kRestartPlayer:
        g_zone.restarted = true;
        break;
    case kHudEnterState:
        player::OnHudEnterState();
        break;
    case kPickup:
        player::OnPickup(object);
        break;
    case kExecuteItem:
        if (params) player::OnExecuteItem(object, params);
        break;
    case kBuildingDamaged:
        if (object && std::find(g_damagedBuildings.begin(), g_damagedBuildings.end(), object) == g_damagedBuildings.end())
            g_damagedBuildings.push_back(object);
        break;
    case kLeaveZone:
        if (g_on.player && g_zone.setupPawn && object && object == engine::LocalPlayerController() && IsZone(engine::GameMode()))
            loadout::Save(object);
        break;
    case kCraftRequest:
    case kDisassembleRequest:
    case kSwapRequest:
    case kEquipRequest: {
        static const char* const kActions[] = { "craft", "disassemble", "swap", "equip" };
        LOG("inventory: %s asks to %s", engine::Name(object).c_str(), kActions[event - kCraftRequest]);
        break;
    }
    }
}

bool Tutorial() {
    uintptr_t world = engine::World();
    return world && engine::Name(world) == "Zone_Onboarding_FarmsteadFort";
}

void Tick() {
    static bool busy = false, patched = false;  // engine calls below come back through ProcessEvent
    static ULONGLONG next = 0;
    if (busy || !engine::Ready()) return;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    busy = true;
    next = now + kTickMs;
    if (!patched) {  // call sites are rewritten on the thread that runs them
        patched = true;
        director::PatchCallSites();
        objectives::PatchDeposit();
        gameplay::PatchCallSites();
        net::PatchCallSites();
    }
    std::vector<std::function<void()>> queue;
    {
        std::lock_guard<std::recursive_mutex> guard(engine::Lock());
        queue.swap(g_queue);
    }
    for (auto& action : queue) action();
    Advance(now);
    busy = false;
}

bool Command(const std::string& cmd, const std::string& arg) {
    if (cmd == "fix") {
        size_t space = arg.find(' ');
        std::string name = arg.substr(0, space), value = space == std::string::npos ? "" : arg.substr(space + 1);
        for (auto& s : kSwitches) {
            if (name == s.name && (value == "on" || value == "off")) *s.on = value == "on";
        }
        std::string all;
        for (auto& s : kSwitches) all += std::string(" ") + s.name + (*s.on ? " on," : " off,");
        LOG("fix:%s", all.substr(0, all.size() - 1).c_str());
    } else if (cmd == "mission") {
        std::wstring path = arg.empty() ? std::wstring(kDefaultMission) : std::wstring(arg.begin(), arg.end());
        g_queue.push_back([path] {
            if (IsZone(engine::GameMode())) LoadMission(path);
            else LOG("game: missions load only in a zone (travel there first)");
        });
    } else if (cmd == "play") {
        trace::Execute(std::string("travel ") + kPlayUrl);
    } else if (cmd == "director") {
        g_queue.push_back([] { director::Spawn(); });
    } else if (cmd == "nav") {
        g_queue.push_back([] { director::ExpandNavigation(); });
    } else if (cmd == "husks") {
        g_queue.push_back([] { director::ListHusks(); });
    } else if (cmd == "spawnhusk") {
        g_queue.push_back([arg] { director::SpawnHusk(arg); });
    } else if (cmd == "host") {
        int port = atoi(arg.c_str());
        g_queue.push_back([port] { if (net::Host(port)) net::Open(); });
    } else if (cmd == "join") {
        if (arg.empty()) net::JoinAny();
        else net::Join(arg);
    } else if (cmd == "netstat") {
        g_queue.push_back([] { net::Status(); });
    } else if (cmd == "bring") {
        g_queue.push_back([] { net::Bring(); });
    } else if (cmd == "outpost") {
        int n = atoi(arg.c_str());
        g_queue.push_back([n] { lobby::Outpost(n); });
    } else if (cmd == "playmission" && !arg.empty()) {
        g_queue.push_back([arg] { lobby::PlayMission(arg); });
    } else if (cmd == "items") {
        g_queue.push_back([] { player::ListItems(); });
    } else if (cmd == "lobbyplay") {
        g_queue.push_back([arg] { lobby::PlayFromTab(arg); });
    } else if (cmd == "drop" && !arg.empty()) {
        g_queue.push_back([arg] { player::ExecuteByName(engine::LocalPlayerController(), arg, true); });
    } else if (cmd == "equip" && !arg.empty()) {
        g_queue.push_back([arg] { player::ExecuteByName(engine::LocalPlayerController(), arg); });
    } else if (cmd == "build") {
        g_queue.push_back([] { net::Build(); });
    } else if (cmd == "walk") {
        net::Walk(atoi(arg.c_str()));
    } else if (cmd == "levels") {
        g_queue.push_back([] { net::ListLevels(); });
    } else if (cmd == "netdormancy") {
        net::SetSkipInitialDormant(arg != "off");
    } else if (cmd == "netactor" && !arg.empty()) {
        g_queue.push_back([arg] { net::Inspect(arg); });
    } else if (cmd == "netdestroyed" && !arg.empty()) {
        g_queue.push_back([arg] { net::InspectDestroyed(arg); });
    } else if (cmd == "editcheck") {
        g_queue.push_back([] { player::EditCheck(); });
    } else if (cmd == "team") {
        g_queue.push_back([] { player::EnsureTeam(engine::LocalPlayerController()); });
    } else if (cmd == "give" && !arg.empty()) {
        size_t space = arg.find(' ');
        std::string path = arg.substr(0, space);
        int count = space == std::string::npos ? 1 : atoi(arg.c_str() + space + 1);
        bool everyone = arg.size() > 4 && arg.compare(arg.size() - 4, 4, " all") == 0;  // the joined players too
        std::wstring wide(path.begin(), path.end());
        g_queue.push_back([wide, count, everyone] { player::Give(wide, count > 0 ? count : 1, everyone); });
    } else if (cmd == "setup") {
        g_queue.push_back([] {
            uintptr_t pc = engine::LocalPlayerController();
            player::Setup(pc, engine::PawnOf(pc));
        });
    } else if (cmd == "abilities") {
        g_queue.push_back([] { player::GrantAbilities(); });
    } else if (cmd == "ui") {
        g_queue.push_back([] { player::ForceInGameUI(); });
    } else if (cmd == "outfit" || cmd == "hero") {
        g_queue.push_back([] {
            uintptr_t pc = engine::LocalPlayerController();
            player::ApplyHero(pc, engine::PawnOf(pc));
        });
    } else if (cmd == "objectives") {
        objectives::List();
    } else if (cmd == "win") {
        g_queue.push_back([] { objectives::ForceSuccess(); });
    } else if (cmd == "endzone") {
        bool won = arg != "lose";
        g_queue.push_back([won] { endzone::Finish(won); });
    } else if (cmd == "leave" || cmd == "lobby") {
        g_queue.push_back([] { endzone::RequestLeave(); });
    } else {
        return false;
    }
    return true;
}

}  // namespace game
