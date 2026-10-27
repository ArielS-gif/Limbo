#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <cmath>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace director {

using engine::At;

// The AI director and goal manager are real code in this client; only their creation and their getters are
// compiled out. The getters were folded into 'xor eax,eax; ret' (00EECAF0, shared by thousands of callers, never
// patched), so each gameplay call site is pointed at a thunk returning what is spawned here (null until then,
// exactly like the stub). Debug and cheat call sites are left alone.
static volatile uintptr_t g_director = 0, g_goalManager = 0;

__declspec(naked) static void ThunkDirector() {
    __asm {
        mov eax, dword ptr [g_director]
        ret
    }
}

__declspec(naked) static void ThunkGoalManager() {
    __asm {
        mov eax, dword ptr [g_goalManager]
        ret
    }
}

namespace {
constexpr uint32_t kNullGetter = 0x00EECAF0;
// Mission encounters (start, from a generated profile, sequences, stop), AI pawn removal, rift register/unregister,
// trap and building notifications, FortGameMode, a goal-manager helper, the patrol-ward EQS test, and the player's
// AI data manager.
const uint32_t kDirectorSites[] = {
    0x00786400, 0x007865E5, 0x0077450D, 0x00787679, 0x0078772E, 0x00807406, 0x00819E3F, 0x006768A6,
    0x006577E3, 0x00658BA7, 0x00636A4E, 0x006F2355, 0x005C93FB, 0x005D484C, 0x00871D20,
};
const uint32_t kGoalManagerSites[] = {
    0x0059D688, 0x0059E246, 0x0059E288, 0x005A40F7, 0x005A6BDA, 0x0059EACC, 0x005B264C, 0x005AA70F, 0x005CD2D8,
    0x0058CD06, 0x005FDF13, 0x007728A5, 0x00772955, 0x0077448A, 0x00775AD8, 0x00784F34, 0x00784FA5, 0x00785015,
    0x0086BFDC,
};
// FortKismetLibrary.GetAIDirector (BP_Atlas registers itself as a patrol ward through it) shares its exec thunk
// with GetAIGoalManager, so only its UFunction's native pointer (+0x74) is wrapped.
constexpr uint32_t kGetterExec = 0x00A2B6B0;
constexpr int FunctionNative = 0x74;

constexpr uint32_t kGEngine = 0x03267DD8;
constexpr int EngineGameSingleton = 0x148, GlobalsEnableNewEncounters = 0x17E;  // UFortGlobals
constexpr int DirectorFlags = 0x7A0;           // bit 2 bRegisteredForDayPhaseChange
constexpr int DirectorEncounterTimer = 0x92C;  // FTimerHandle of the encounter update, -1 until WorldReady
// Patrols ("PATrolls"): husk groups roaming the map besides the objective's waves (EDO_Set_Patrol, EQS_EncounterPatrol*).
// The director's blueprint starts them in OnStartSpawningPATrolls when its bSpawnPATrolls (+0x8EC) is switched on; what
// switched it on is server code that is compiled out. FortCheatManager.ToggleSpawnPATrolls's toggle is used instead.
constexpr uint32_t kTogglePatrols = 0x005B1E70;  // UFortAIDirector: bool (), thiscall: flips the flag, fires the event
constexpr int DirectorSpawnPatrols = 0x8EC;
// Chest guardians: SpawnGuardians gathers every BuildingContainer whose byte at +0x974 is set (the tiered chests) and
// hands them to OnReadyToSpawnGuardians, where the director's blueprint places husks by each one (SpawnAIFromClass, in
// the same call). The director's WorldReady only calls it when the game is started with -guardians, so this build had
// no husks on the map until the Atlas started; it is called here instead, once per director.
constexpr uint32_t kSpawnGuardians = 0x005AF180;  // UFortAIDirector, thiscall, no arguments
// The zone's only navigation bounds are UnitNavMeshBounds, a 1-uu NavMeshBoundsVolume at the origin in the always-loaded
// sublevel /Game/Maps/NavMeshBounds. The server code that sized it is absent, so the husk navmesh stays empty and every
// rift query fails ("Encounter fallback EQS Query failed! Aborting the encounter!"). After scaling the volume over the
// zone, UNavigationSystem::RebuildAll re-gathers the bounds, recreates each navmesh generator for the new box and marks
// every tile dirty in the call; the tiles then build in the background (Build() would do it synchronously, freezing the
// game for ~1500 tiles).
constexpr int WorldNavigationSystem = 0x90;
constexpr int NavMainNavData = 0x1C, NavBuildLock = 0x2A4;  // lock bit 2: the initial lock, released only in the editor
constexpr int NavDataSet = 0x44;                // TArray<ANavigationData*>
constexpr int NavPendingBoundsUpdates = 0x9C;   // PendingNavBoundsUpdates.Num
constexpr int NavDirtyAreas = 0x284;            // DirtyAreas.Num
constexpr int NavDataGenerator = 0x2A8;         // ARecastNavMesh's FRecastNavMeshGenerator
constexpr int GeneratorPendingTiles = 0xD0, GeneratorRunningTiles = 0xDC;  // Num of each tile array
constexpr uint32_t kRebuildAll = 0x0153A8C0;    // UNavigationSystem::RebuildAll, thiscall, no args
constexpr float kZoneNavScale[3] = { 28672.0f, 28672.0f, 8192.0f };  // the 1-uu box at the origin: +-14336 covers 2x2 tiles

uintptr_t NavigationSystem() {
    uintptr_t world = engine::World();
    uintptr_t nav = world ? At<uint32_t>(world, WorldNavigationSystem) : 0;
    return engine::IsA(nav, "NavigationSystem") ? nav : 0;
}

// AFortAIController::Possess is compiled out: its vtable entry (FortAIController vtable 026799EC, slot 0x308) points at a
// shared 'ret 4', so the director's husks and their controllers never join and the husks stand still. The entry is
// pointed at the engine's AAIController::Possess, then the pawn's own behavior tree is started, as the override did.
constexpr uint32_t kAIPossessEntry = 0x02679CF4, kPossessStub = 0x019E8750;
constexpr uint32_t kAIControllerPossess = 0x013A2EF0;  // AAIController::Possess(APawn*), thiscall, ret 4
constexpr int AIPawnBehaviorTree = 0x8E4;              // AFortAIPawn

// Husk goals. The goal manager gives each AI a goal as its Tick (005D9490) walks the AIs in its list (a map at +0x268),
// but whatever put a controller in that list is compiled out, so no husk ever gets one and BT_HuskBase only sleeps
// ("Has NO Goal Actor?"). The possessed husks are followed here instead and given a goal through
// FortAIController.SetGoalActor with its location always known ("Goal is Always Perceived": no hunting): the player
// when close, else the mission's Atlas. The tree then walks them there (BT_MoveToWithUndermining, breaking through
// what is in the way) and picks their melee attacks. MyFortPawn is set by the controller's own SetPawn override.
constexpr int ControllerGoalActor = 0x2F8, ControllerMyFortPawn = 0x358;    // AFortAIController
constexpr int ControllerPerception = 0x2B0, ControllerBlackboard = 0x2B8;  // AAIController
constexpr int ControllerGoalActorKey = 0x34D;                               // blackboard key ids cached at +0x34C..+0x357
constexpr int PawnIsDying = 0x5B8, PawnAbilitySystem = 0x770;              // AFortPawn
constexpr int AbilitySystemAbilityCount = 0x1A0 + 0x80 + 4;                // ActivatableAbilities.Items.Num
constexpr int ObjectName = 0x10, ObjectNameNumber = 0x14;
constexpr float kChaseRange = 1500.0f, kGiveUpRange = 2500.0f;  // a husk this close goes for the player, until this far
constexpr float kRoamerNotice = 2500.0f, kRoamerGiveUp = 4000.0f;  // the same for the groups out on the map
constexpr float kRoamerFar = 14000.0f;  // a group this far from every player for a minute is taken away
// The Atlas pulls the husks within this distance (the encounter's rifts are near it); a patrol farther off waits where
// it is and goes for the player within kGiveUpRange.
constexpr float kAtlasPullRange = 15000.0f;
constexpr ULONGLONG kGoalUpdateMs = 500, kGoalSummaryMs = 15000;

struct Husk {
    uintptr_t controller;
    int32_t name, number;  // a later object at a recycled address has another name
    bool guard;            // a chest's guardian: it stays by the chest unless the player comes close
    bool roamer;           // one of the groups out on the map: waits where it is until a player comes close
    ULONGLONG farSince;    // a roamer far from every player since then (0: not far)
};
std::vector<Husk> g_husks;
ULONGLONG g_nextGoals = 0, g_nextSummary = 0;
int g_goalsLogged = 0;
bool g_spawningGuardians = false, g_guardiansSpawned = false, g_spawningRoamers = false;
int g_guards = 0;
ULONGLONG g_roamersFrom = 0, g_nextRoamers = 0;  // the groups out on the map (UpdateRoamers)
int g_roamerGroupsSpawned = 0;

void Track(uintptr_t controller) {
    int32_t name = At<int32_t>(controller, ObjectName), number = At<int32_t>(controller, ObjectNameNumber);
    g_guards += g_spawningGuardians;
    for (auto& husk : g_husks) {
        if (husk.controller == controller) {
            husk = { controller, name, number, g_spawningGuardians, g_spawningRoamers, 0 };
            return;
        }
    }
    g_husks.push_back({ controller, name, number, g_spawningGuardians, g_spawningRoamers, 0 });
}

bool Alive(uintptr_t pawn, float* at) {
    return engine::IsLive(pawn) && !At<uint8_t>(pawn, PawnIsDying) && engine::Location(pawn, at);
}

float Distance(const float* a, const float* b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int AbilityCount(uintptr_t pawn) {
    uintptr_t abilitySystem = At<uint32_t>(pawn, PawnAbilitySystem);
    return abilitySystem ? At<int32_t>(abilitySystem, AbilitySystemAbilityCount) : -1;
}

void SetGoal(uintptr_t controller, uintptr_t goal) {
    struct {
        uintptr_t actor;
        bool locationAlwaysKnown;
        uint8_t pad[3];
    } p{ goal, true };
    engine::ProcessEvent(controller, engine::FindFunction("/Script/FortniteGame.FortAIController.SetGoalActor"), &p);
}

void __fastcall FortAIControllerPossess(uintptr_t controller, void*, uintptr_t pawn) {
    reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kAIControllerPossess))(controller, pawn);
    if (!game::g_on.director || !engine::IsA(pawn, "FortAIPawn")) return;
    uintptr_t tree = At<uint32_t>(pawn, AIPawnBehaviorTree);
    if (!tree) return;
    struct {
        uintptr_t tree;
        bool ok;
        uint8_t pad[3];
    } p{ tree, false };
    engine::ProcessEvent(controller, engine::FindFunction("/Script/AIModule.AIController.RunBehaviorTree"), &p);
    if (p.ok) Track(controller);
    static int logged = 0;
    if (logged++ < 3) LOG("director: %s possessed %s, behavior tree %s %s", engine::Name(controller).c_str(),
                          engine::Name(pawn).c_str(), engine::Name(tree).c_str(), p.ok ? "running" : "NOT started");
}

void __fastcall ExecGetAIDirector(uintptr_t context, void*, void* stack, void* result) {
    reinterpret_cast<void(__thiscall*)(uintptr_t, void*, void*)>(engine::Va(kGetterExec))(context, stack, result);
    *static_cast<uint32_t*>(result) = static_cast<uint32_t>(g_director);
}
}  // namespace

bool Install() {
    bool possess = engine::PatchPointer(kAIPossessEntry, kPossessStub, reinterpret_cast<void*>(&FortAIControllerPossess));
    LOG("director: FortAIController.Possess %s", possess ? "restored" : "NOT restored");
    uintptr_t function = engine::FindFunction("/Script/FortniteGame.FortKismetLibrary.GetAIDirector");
    if (!function || At<uint32_t>(function, FunctionNative) != engine::Va(kGetterExec)) {
        LOG("director: FortKismetLibrary.GetAIDirector not found or not the expected native: left alone");
        return false;
    }
    At<uint32_t>(function, FunctionNative) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ExecGetAIDirector));
    LOG("director: FortKismetLibrary.GetAIDirector wrapped");
    return possess;
}

bool PatchCallSites() {
    int done = 0, total = 0;
    for (uint32_t site : kDirectorSites) {
        total++;
        done += engine::PatchCall(site, kNullGetter, reinterpret_cast<void*>(&ThunkDirector));
    }
    for (uint32_t site : kGoalManagerSites) {
        total++;
        done += engine::PatchCall(site, kNullGetter, reinterpret_cast<void*>(&ThunkGoalManager));
    }
    LOG("director: %d of %d getter call sites now return the spawned director / goal manager", done, total);
    return done == total;
}

void Reset() {
    g_director = 0;
    g_goalManager = 0;
    g_husks.clear();
    g_nextGoals = g_nextSummary = 0;
    g_goalsLogged = 0;
    g_spawningGuardians = g_guardiansSpawned = g_spawningRoamers = false;
    g_guards = 0;
    g_roamersFrom = g_nextRoamers = 0;
    g_roamerGroupsSpawned = 0;
}

void Check() {
    if (g_director && !engine::IsLive(g_director)) {
        LOG("director: the AI director is gone");
        g_director = 0;
    }
    if (g_goalManager && !engine::IsLive(g_goalManager)) g_goalManager = 0;
}

void Spawn() {
    if (g_director) {
        LOG("director: already spawned (%s)", engine::Describe(g_director).c_str());
        return;
    }
    uintptr_t goalClass = engine::Load(L"/Game/AI/GoalSelection/AIGoalManager.AIGoalManager_C");
    uintptr_t directorClass = engine::Load(L"/Game/AIDirector/AIDirector_Fortnite.AIDirector_Fortnite_C");
    if (!engine::IsChildOf(goalClass, "FortAIGoalManager") || !engine::IsChildOf(directorClass, "FortAIDirector")) {
        LOG("director: unexpected classes (%s, %s)", engine::Describe(goalClass).c_str(), engine::Describe(directorClass).c_str());
        return;
    }
    if (!g_goalManager) g_goalManager = engine::SpawnActor(goalClass, 0);
    g_director = engine::SpawnActor(directorClass, 0);  // registers itself for WorldReady in PostInitializeComponents
    LOG("director: spawned %s and %s", engine::Describe(g_goalManager).c_str(), engine::Describe(g_director).c_str());
}

bool Spawned() {
    return g_director != 0;
}

bool Ready() {
    uintptr_t director = g_director;
    return director && (At<uint8_t>(director, DirectorFlags) & 4) && At<int32_t>(director, DirectorEncounterTimer) != -1;
}

void StartAmbient() {
    uintptr_t director = g_director;
    if (!director || !engine::IsLive(director)) return;
    if (!At<uint8_t>(director, DirectorSpawnPatrols)) {
        bool on = reinterpret_cast<bool(__thiscall*)(uintptr_t)>(engine::Va(kTogglePatrols))(director);
        LOG("director: patrols %s", on ? "on: husk groups roam the map" : "still off");
    }
    if (g_guardiansSpawned) return;
    g_guardiansSpawned = true;
    int before = g_guards;
    g_spawningGuardians = true;
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(engine::Va(kSpawnGuardians))(director);
    g_spawningGuardians = false;
    LOG("director: chest guardians: %d husk(s) placed by the chests", g_guards - before);
}

void UpdateGoals() {
    ULONGLONG now = GetTickCount64();
    if (!game::g_on.director || g_husks.empty() || now < g_nextGoals) return;
    g_nextGoals = now + kGoalUpdateMs;
    // Every player in the zone (co-op: the host's pawn and the joined players'); each husk goes for the nearest.
    struct Player {
        uintptr_t pawn;
        float at[3];
    };
    std::vector<Player> players;
    for (uintptr_t pawn : net::PlayerPawns()) {
        Player p{ pawn };
        if (Alive(pawn, p.at)) players.push_back(p);
    }
    uintptr_t atlasGoal = atlas::Goal();
    float atlasAt[3];
    bool atlasKnown = atlasGoal && engine::Location(atlasGoal, atlasAt);
    int onPlayer = 0, onAtlas = 0, idle = 0;
    for (size_t i = 0; i < g_husks.size();) {
        uintptr_t controller = g_husks[i].controller;
        if (!engine::IsLive(controller) || At<int32_t>(controller, ObjectName) != g_husks[i].name ||
            At<int32_t>(controller, ObjectNameNumber) != g_husks[i].number) {
            g_husks.erase(g_husks.begin() + i);
            continue;
        }
        bool guard = g_husks[i].guard, roamer = g_husks[i].roamer;
        Husk& husk = g_husks[i];
        i++;
        uintptr_t pawn = At<uint32_t>(controller, ControllerMyFortPawn);
        float at[3];
        if (!Alive(pawn, at)) continue;
        uintptr_t current = At<uint32_t>(controller, ControllerGoalActor);
        // The nearest player; the one it is chasing counts as nearer within the give-up range, so it doesn't flip.
        uintptr_t player = 0;
        float toPlayer = 0.0f, best = 0.0f;
        for (const Player& p : players) {
            float d = Distance(at, p.at), score = p.pawn == current && d < kGiveUpRange ? 0.0f : d;
            if (!player || score < best) {
                player = p.pawn;
                best = score;
                toPlayer = d;
            }
        }
        uintptr_t goal = 0;
        if (roamer) {
            husk.farSince = player && toPlayer < kRoamerFar ? 0 : husk.farSince ? husk.farSince : now;
            if (player && (toPlayer < kRoamerNotice || (current == player && toPlayer < kRoamerGiveUp))) goal = player;
        } else if (player && (toPlayer < kChaseRange || (current == player && toPlayer < kGiveUpRange))) goal = player;
        else if (guard) goal = 0;  // back to waiting where it is
        else if (atlasGoal && (!atlasKnown || Distance(at, atlasAt) < kAtlasPullRange)) goal = atlasGoal;
        else if (player && toPlayer < kGiveUpRange) goal = player;
        if (!goal) idle++;
        else if (goal == player) onPlayer++;
        else onAtlas++;
        if (goal == current) continue;
        SetGoal(controller, goal);
        if (g_goalsLogged < 4) {
            g_goalsLogged++;
            LOG("director: %s (%s, %d abilities) -> goal %s, %.0f away; now %s (perception %s, blackboard %s, key %d)",
                engine::Name(controller).c_str(), engine::Name(pawn).c_str(), AbilityCount(pawn), engine::Name(goal).c_str(),
                goal == player ? toPlayer : 0.0f, engine::Name(At<uint32_t>(controller, ControllerGoalActor)).c_str(),
                engine::Describe(At<uint32_t>(controller, ControllerPerception)).c_str(),
                engine::Name(At<uint32_t>(controller, ControllerBlackboard)).c_str(), At<uint8_t>(controller, ControllerGoalActorKey));
        }
    }
    if (now >= g_nextSummary && onPlayer + onAtlas + idle > 0) {
        g_nextSummary = now + kGoalSummaryMs;
        LOG("director: %d husk(s): %d after the player, %d on the Atlas, %d idle", onPlayer + onAtlas + idle, onPlayer,
            onAtlas, idle);
    }
}

void ListHusks() {
    LOG("director: %zu husk(s) followed; Atlas %s", g_husks.size(), engine::Describe(atlas::Goal()).c_str());
    for (auto& husk : g_husks) {
        uintptr_t controller = husk.controller;
        if (!engine::IsLive(controller)) continue;
        uintptr_t pawn = At<uint32_t>(controller, ControllerMyFortPawn), goal = At<uint32_t>(controller, ControllerGoalActor);
        float at[3] = {}, goalAt[3] = {};
        bool alive = Alive(pawn, at);
        bool located = goal && engine::Location(goal, goalAt);
        LOG("  %s: %s%s %s at (%.0f, %.0f, %.0f), %d abilities, goal %s%s", engine::Name(controller).c_str(),
            engine::Name(pawn).c_str(), husk.guard ? " (chest guardian)" : "", alive ? "alive" : "dead/gone", at[0], at[1],
            at[2], pawn ? AbilityCount(pawn) : -1, engine::Name(goal).c_str(),
            located ? (" " + std::to_string(static_cast<int>(Distance(at, goalAt))) + " away").c_str() : "");
    }
}

namespace {
// The enemy blueprints under /Game/Characters/Enemies/, by a short name for `spawnhusk`.
struct HuskKind {
    const char* name;
    const wchar_t* folder;
    const wchar_t* asset;
};
const HuskKind kHuskKinds[] = {
    { "husk", L"Husk", L"HuskPawn" },
    { "fire", L"Husk", L"HuskPawn_Fire" },
    { "ice", L"Husk", L"HuskPawn_Ice" },
    { "lightning", L"Husk", L"HuskPawn_Lightning" },
    { "pitcher", L"Husk", L"HuskPawn_Pitcher" },
    { "husky", L"Husk", L"HuskPawn_Husky" },
    { "husky_fire", L"Husk", L"HuskPawn_Husky_Fire" },
    { "husky_ice", L"Husk", L"HuskPawn_Husky_Ice" },
    { "husky_lightning", L"Husk", L"HuskPawn_Husky_Lightning" },
    { "dwarf", L"Husk", L"HuskPawn_Dwarf" },
    { "dwarf_fire", L"Husk", L"HuskPawn_Dwarf_Fire" },
    { "dwarf_ice", L"Husk", L"HuskPawn_Dwarf_Ice" },
    { "dwarf_lightning", L"Husk", L"HuskPawn_Dwarf_Lightning" },
    { "beehive", L"Husk", L"HuskPawn_Beehive" },
    { "bombshell", L"Husk", L"HuskPawn_Bombshell" },
    { "poison", L"Husk", L"HuskPawn_Bombshell_Poison" },
    { "sploder", L"Husk", L"HuskPawn_Sploder" },
    { "trashman", L"Husk", L"HuskPawn_Trashman" },
    { "blaster", L"Husk", L"BlasterPawn" },
    { "flinger", L"Flinger", L"FlingerPawn" },
    { "smasher", L"Smasher", L"SmasherPawn" },
    { "smasher_fire", L"Smasher", L"SmasherPawn_Fire" },
    { "smasher_ice", L"Smasher", L"SmasherPawn_Ice" },
    { "smasher_lightning", L"Smasher", L"SmasherPawn_Lightning" },
    { "smasher_boss", L"Smasher", L"SmasherPawn_Boss" },
    { "taker", L"Taker", L"TakerPawn" },
    { "troll", L"Thief", L"TrollPawn" },
    { "werewolf", L"Transylvania/Werewolf", L"WerewolfPawn" },
};
constexpr float kSpawnAhead = 600.0f, kSpawnSpacing = 150.0f, kSpawnLift = 50.0f;
constexpr int kMaxSpawnCount = 10;
constexpr uint8_t kAdjustButAlwaysSpawn = 2;
}  // namespace

void SpawnHusk(const std::string& arg) {
    size_t space = arg.find(' ');
    std::string type = arg.substr(0, space);
    int count = space == std::string::npos ? 1 : atoi(arg.c_str() + space + 1);
    count = count < 1 ? 1 : count > kMaxSpawnCount ? kMaxSpawnCount : count;
    const HuskKind* kind = nullptr;
    std::string names;
    for (auto& k : kHuskKinds) {
        if (type == k.name) kind = &k;
        names += std::string(" ") + k.name;
    }
    if (!kind) {
        LOG("director: spawnhusk <type> [count], %d at most; types:%s", kMaxSpawnCount, names.c_str());
        return;
    }
    uintptr_t pawn = engine::PawnOf(engine::LocalPlayerController());
    float at[3];
    if (!Alive(pawn, at)) {
        LOG("director: spawnhusk needs the player's pawn in a zone");
        return;
    }
    std::wstring path = std::wstring(L"/Game/Characters/Enemies/") + kind->folder + L"/Blueprints/" + kind->asset + L"." +
                        kind->asset + L"_C";
    uintptr_t cls = engine::Load(path);
    if (!engine::IsChildOf(cls, "FortAIPawn")) {
        LOG("director: %ls is not a husk class (%s)", path.c_str(), engine::Describe(cls).c_str());
        return;
    }
    // In a row across the player's view, kSpawnAhead in front, facing the player.
    float forward[3] = {};
    engine::ProcessEvent(pawn, engine::FindFunction("/Script/Engine.Actor.GetActorForwardVector"), forward);
    float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
    if (length < 0.01f) forward[0] = 1.0f, forward[1] = 0.0f, length = 1.0f;
    forward[0] /= length;
    forward[1] /= length;
    float rotation[3] = { 0.0f, std::atan2(-forward[1], -forward[0]) * 57.2957795f, 0.0f };
    uintptr_t spawnController = engine::FindFunction("/Script/Engine.Pawn.SpawnDefaultController");
    int spawned = 0;
    for (int i = 0; i < count; i++) {
        float side = (i - (count - 1) * 0.5f) * kSpawnSpacing;
        float location[3] = { at[0] + forward[0] * kSpawnAhead - forward[1] * side,
                              at[1] + forward[1] * kSpawnAhead + forward[0] * side, at[2] + kSpawnLift };
        uintptr_t husk = engine::SpawnActorAt(cls, 0, location, rotation, kAdjustButAlwaysSpawn);
        if (!husk) continue;
        engine::ProcessEvent(husk, spawnController, nullptr);  // an AI controller possesses it, as SpawnAIFromClass does
        spawned++;
    }
    LOG("director: spawned %d of %d %s (%ls)", spawned, count, kind->name, kind->asset);
}

namespace {
// Husk groups out on the map. The director's own patrols (OnStartSpawningPATrolls, switched on in StartAmbient) start
// but never place a husk in this build, so a zone had none outside the chests' guardians and the Atlas's waves. Groups
// are placed here instead: on the navmesh, somewhere a player could walk to, out of every player's sight range, more
// of them with more players. A group waits where it is until a player comes within kRoamerNotice, then hunts them
// (UpdateGoals); one left far behind by every player is taken away, and new ones come while the zone runs. Mostly
// plain husks, with pitchers, huskies, dwarfs, beehives, sploders and bombshells; now and then an elemental group,
// a smasher or a blaster. Only kinds whose behavior tree goes for the goal given here (BT_HuskBase, BT_HuskRanged,
// BT_Smasher): the flinger's BT_Flinger circles its goal-manager assignment and throws the husks around it, which
// nothing hands out here, so a flinger on its own stood still even with a player beside it; the taker's BT_Taker
// wasn't seen to attack either. (The troll and the werewolf come without abilities in this build; the boss smasher
// isn't used.)
struct RoamerGroup {
    int weight;
    const char* pool[5];  // each member is drawn from these (a null entry ends the pool)
    int minSize, maxSize;
};
const RoamerGroup kRoamerGroups[] = {
    { 36, { "husk", "husk", "husk", "pitcher" }, 3, 5 },
    { 18, { "husk", "husky", "dwarf", "pitcher", "beehive" }, 3, 5 },
    { 12, { "husk", "sploder", "bombshell", "poison", "trashman" }, 3, 4 },
    { 7, { "fire", "husky_fire", "dwarf_fire" }, 2, 4 },
    { 7, { "ice", "husky_ice", "dwarf_ice" }, 2, 4 },
    { 7, { "lightning", "husky_lightning", "dwarf_lightning" }, 2, 4 },
    { 5, { "smasher" }, 1, 1 },
    { 3, { "blaster" }, 1, 2 },
};
constexpr int kRoamersBase = 8, kRoamersPerPlayer = 4, kRoamersMax = 24;  // husks out on the map at once
constexpr ULONGLONG kRoamerFirstMs = 30000, kRoamerGroupMs = 20000, kRoamerFarMs = 60000;
// A spot kRoamerSpawnNear..kRoamerSpawnFar from a player, on the navmesh, kRoamerSpawnMin from every player.
constexpr float kRoamerSpawnMin = 3500.0f, kRoamerSpawnNear = 4000.0f, kRoamerSpawnFar = 7000.0f, kRoamerMemberSpacing = 160.0f;
constexpr float kRoamerProjectXY = 600.0f, kRoamerProjectZ = 4000.0f, kRoamerLift = 100.0f;
constexpr int kRoamerTries = 12;
uint32_t g_random = 0;

uint32_t Random(uint32_t below) {
    if (!g_random) g_random = static_cast<uint32_t>(GetTickCount64()) | 1;
    g_random = g_random * 1664525u + 1013904223u;
    return below ? (g_random >> 8) % below : 0;
}

uintptr_t KindClass(const char* name) {
    for (auto& k : kHuskKinds) {
        if (std::string(name) != k.name) continue;
        std::wstring path = std::wstring(L"/Game/Characters/Enemies/") + k.folder + L"/Blueprints/" + k.asset + L"." + k.asset + L"_C";
        uintptr_t cls = engine::Load(path);
        return engine::IsChildOf(cls, "FortAIPawn") ? cls : 0;
    }
    return 0;
}

// A point on the navmesh near this one: NavigationSystem.ProjectPointToNavigation(WorldContext, Point, NavData,
// FilterClass, QueryExtent) -> FVector, the point itself when nothing is in reach. (GetRandomReachablePointInRadius
// gave the same spot every time here.)
bool OnNavigation(const float* point, float* out) {
    static uintptr_t function = engine::FindFunction("/Script/Engine.NavigationSystem.ProjectPointToNavigation");
    uintptr_t defaults = engine::FindObject("/Script/Engine.Default__NavigationSystem", "NavigationSystem");
    if (!function || !defaults) return false;
    struct {
        uintptr_t context;
        float point[3];
        uintptr_t navData, filter;
        float extent[3];
        float result[3];
    } p{ engine::GameMode(), { point[0], point[1], point[2] }, 0, 0, { kRoamerProjectXY, kRoamerProjectXY, kRoamerProjectZ }, {} };
    engine::ProcessEvent(defaults, function, &p);
    if (p.result[0] == point[0] && p.result[1] == point[1] && p.result[2] == point[2]) return false;
    if (p.result[0] == 0.0f && p.result[1] == 0.0f && p.result[2] == 0.0f) return false;
    for (int k = 0; k < 3; k++) out[k] = p.result[k];
    return true;
}

using PlayerSpots = std::vector<std::array<float, 3>>;

int SpawnRoamerGroup(const PlayerSpots& players) {
    float spot[3];
    bool found = false;
    for (int t = 0; t < kRoamerTries && !found; t++) {
        const auto& from = players[Random(static_cast<uint32_t>(players.size()))];
        float angle = static_cast<float>(Random(3600)) * 0.00174533f;
        float distance = kRoamerSpawnNear + static_cast<float>(Random(static_cast<uint32_t>(kRoamerSpawnFar - kRoamerSpawnNear)));
        float guess[3] = { from[0] + std::cos(angle) * distance, from[1] + std::sin(angle) * distance, from[2] };
        if (!OnNavigation(guess, spot)) continue;
        found = true;
        for (const auto& p : players)
            if (Distance(spot, p.data()) < kRoamerSpawnMin) found = false;
    }
    if (!found) return 0;
    int total = 0;
    for (const auto& g : kRoamerGroups) total += g.weight;
    int pick = static_cast<int>(Random(static_cast<uint32_t>(total)));
    const RoamerGroup* group = &kRoamerGroups[0];
    for (const auto& g : kRoamerGroups) {
        if (pick < g.weight) {
            group = &g;
            break;
        }
        pick -= g.weight;
    }
    int poolSize = 0;
    while (poolSize < 5 && group->pool[poolSize]) poolSize++;
    int size = group->minSize + static_cast<int>(Random(static_cast<uint32_t>(group->maxSize - group->minSize + 1)));
    uintptr_t spawnController = engine::FindFunction("/Script/Engine.Pawn.SpawnDefaultController");
    float yaw = static_cast<float>(Random(360));
    float rotation[3] = { 0.0f, yaw, 0.0f };
    std::string names;
    int spawned = 0;
    g_spawningRoamers = true;
    for (int i = 0; i < size; i++) {
        const char* kind = group->pool[Random(static_cast<uint32_t>(poolSize))];
        uintptr_t cls = KindClass(kind);
        if (!cls) continue;
        // Around the spot, each on the navmesh too (on a slope a point beside it can be inside the ground).
        float angle = (i * 360.0f / size + yaw) * 0.0174533f;
        float beside[3] = { spot[0] + std::cos(angle) * kRoamerMemberSpacing, spot[1] + std::sin(angle) * kRoamerMemberSpacing, spot[2] };
        float at[3] = { spot[0], spot[1], spot[2] };
        if (i) OnNavigation(beside, at);
        at[2] += kRoamerLift;
        uintptr_t husk = engine::SpawnActorAt(cls, 0, at, rotation, kAdjustButAlwaysSpawn);
        if (!husk) continue;
        engine::ProcessEvent(husk, spawnController, nullptr);  // possessed: tracked as a roamer
        names += std::string(" ") + kind;
        spawned++;
    }
    g_spawningRoamers = false;
    float nearest = 0.0f;
    for (const auto& p : players) {
        float d = Distance(spot, p.data());
        if (!nearest || d < nearest) nearest = d;
    }
    if (spawned) {
        g_roamerGroupsSpawned++;
        LOG("director: group %d out on the map at (%.0f, %.0f, %.0f), %.0f from the nearest player:%s", g_roamerGroupsSpawned,
            spot[0], spot[1], spot[2], nearest, names.c_str());
    }
    return spawned;
}
}  // namespace

void UpdateRoamers(bool active) {
    ULONGLONG now = GetTickCount64();
    if (!game::g_on.director || !game::g_on.roamers || !active || !g_director || !Ready()) {
        g_roamersFrom = 0;
        return;
    }
    if (!g_roamersFrom) g_roamersFrom = now + kRoamerFirstMs;
    // Roamers every player left far behind go away (pawn and controller).
    int alive = 0;
    for (auto& husk : g_husks) {
        if (!husk.roamer || !engine::IsLive(husk.controller)) continue;
        uintptr_t pawn = At<uint32_t>(husk.controller, ControllerMyFortPawn);
        float at[3];
        if (!Alive(pawn, at)) continue;
        if (husk.farSince && now - husk.farSince > kRoamerFarMs) {
            uintptr_t destroy = engine::FindFunction("/Script/Engine.Actor.K2_DestroyActor");
            engine::ProcessEvent(pawn, destroy, nullptr);
            engine::ProcessEvent(husk.controller, destroy, nullptr);
            husk.farSince = 0;
            continue;
        }
        alive++;
    }
    if (now < g_roamersFrom || now < g_nextRoamers) return;
    PlayerSpots players;
    for (uintptr_t pawn : net::PlayerPawns()) {
        std::array<float, 3> at{};
        if (Alive(pawn, at.data())) players.push_back(at);
    }
    if (players.empty()) return;
    int target = kRoamersBase + kRoamersPerPlayer * static_cast<int>(players.size());
    if (target > kRoamersMax) target = kRoamersMax;
    if (alive >= target) return;
    g_nextRoamers = now + kRoamerGroupMs;
    if (!SpawnRoamerGroup(players)) g_nextRoamers = now + kRoamerGroupMs / 4;  // no spot this time: soon again
}

// Mission_Gates' encounter otherwise goes through generated encounter profiles, which this client may not be able
// to build; with the flag off it starts BaseGateEncounter_C, which needs only the director.
void UseOldEncounters() {
    uintptr_t gEngine = At<uint32_t>(engine::Va(kGEngine), 0);
    uintptr_t globals = gEngine ? At<uint32_t>(gEngine, EngineGameSingleton) : 0;
    if (!engine::IsA(globals, "FortGlobals")) {
        LOG("director: FortGlobals not found (%s)", engine::Describe(globals).c_str());
        return;
    }
    At<uint8_t>(globals, GlobalsEnableNewEncounters) = 0;
}

bool ExpandNavigation() {
    uintptr_t nav = NavigationSystem();
    uintptr_t volume = engine::FindFirstOfClass("NavMeshBoundsVolume");
    if (!nav || !volume) {
        LOG("director: navigation system %s, bounds volume %s: can't build the navmesh", engine::Describe(nav).c_str(),
            engine::Describe(volume).c_str());
        return false;
    }
    if (At<int32_t>(nav, NavDataSet + 4) < 1) {
        LOG("director: no navigation data in this world; RebuildAll would not create any");
        return false;
    }
    float scale[3] = { kZoneNavScale[0], kZoneNavScale[1], kZoneNavScale[2] };
    engine::ProcessEvent(volume, engine::FindFunction("/Script/Engine.Actor.SetActorScale3D"), scale);
    At<uint8_t>(nav, NavBuildLock) &= ~4;  // only the initial lock; rifts wait for this byte to be 0
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(engine::Va(kRebuildAll))(nav);
    LOG("director: navigation bounds %s scaled over the zone; the navmesh (%s) rebuilds in the background, %d item(s) "
        "pending, lock %02X", engine::Describe(volume).c_str(), engine::Describe(At<uint32_t>(nav, NavMainNavData)).c_str(),
        NavigationPending(), At<uint8_t>(nav, NavBuildLock));
    return true;
}

int NavigationPending() {
    uintptr_t nav = NavigationSystem();
    if (!nav) return -1;
    int pending = At<int32_t>(nav, NavPendingBoundsUpdates) + At<int32_t>(nav, NavDirtyAreas);
    uintptr_t data = At<uint32_t>(nav, NavDataSet);
    int32_t count = At<int32_t>(nav, NavDataSet + 4);
    for (int32_t i = 0; data && i < count && i < 8; i++) {
        uintptr_t navData = At<uint32_t>(data, i * 4);
        uintptr_t generator = navData ? At<uint32_t>(navData, NavDataGenerator) : 0;
        if (generator) pending += At<int32_t>(generator, GeneratorPendingTiles) + At<int32_t>(generator, GeneratorRunningTiles);
    }
    return pending;
}

}  // namespace director
