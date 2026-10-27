#include "trace.h"
#include "dump.h"
#include "engine.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "memory.h"
#include "mission.h"
#include "ue.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace trace {

// UObject::ProcessEvent (exe+0x9A9E70, matched against Blaze and Alpharium in step 1). Its first three
// instructions, push ebp / mov ebp,esp / sub esp,84h, move to the trampoline.
constexpr uint32_t kProcessEvent = 0x9A9E70;
constexpr uint8_t kProcessEventStart[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x84, 0x00, 0x00, 0x00 };

// Field offsets from Blaze's Dumper-7 SDK of this exe.
namespace off {
constexpr int WorldGameState = 0x34, WorldAuthorityGameMode = 0x94, WorldOwningGameInstance = 0xB0;
constexpr int GameInstanceLocalPlayers = 0x24;  // TArray<ULocalPlayer*>
constexpr int PlayerController = 0x20;          // UPlayer
constexpr int PlayerControllerCheatManager = 0x2F0;
constexpr int GameModeMatchState = 0x240;       // FName; padding in the SDK, so a guess until it reads sensibly
constexpr int FortGameModeZoneInstanceId = 0x2F4, FortGameModeMissionManagerClass = 0x360;
constexpr int FortGameModePendingTimerState = 0x374;
constexpr int FortGameModeReadyBits = 0x37C;    // bit 0 bWorldIsReady, bit 1 bTheaterDataIsReady
constexpr int FortGameStateWorldLevel = 0x2A0, FortGameStateMissionManager = 0x2CC;
constexpr int FortGameStateWorldManager = 0x328, FortGameStateGameplayState = 0x330;
constexpr int FortPlayerControllerSpawnBits = 0x4A4;  // bit 3 bHasInitiallySpawned, 5 bReadyToStartMatch, 6 bClientPawnIsLoaded
constexpr int FortPlayerControllerClientLoaded = 0x510, FortPlayerControllerServerLoaded = 0x511;  // bool each
constexpr int WorldManagerState = 0x268, WorldManagerCurrentWorldRecord = 0x41C;  // state: EFortWorldManagerState
constexpr int MissionManagerMissions = 0x268;   // TArray<AFortMission*>
// UFortWorldRecord::ZoneInstanceInfo (FFortZoneInstanceDetails at 0xE4), what world/validate fills in.
// Soft class pointers are 0x18 bytes with the asset path FString at +0xC.
constexpr int RecordWorldId = 0xE4, RecordTheaterId = 0xF0, RecordTheaterMissionId = 0xFC;
constexpr int RecordZoneThemeClass = 0x108, RecordMissionGenerator = 0x120, RecordTileIndex = 0x144;
constexpr int SoftPtrPath = 0xC;
constexpr int ActorRootComponent = 0x114, SceneComponentRelativeLocation = 0x120;
}
// UObject* __cdecl StaticLoadObject(UClass*, UObject* Outer, const TCHAR* Name, const TCHAR* Filename,
// uint32 LoadFlags, UPackageMap* Sandbox, bool bAllowObjectReconciliation) (Alpharium's offset, file VA 00DAFBD0).
constexpr uint32_t kStaticLoadObject = 0x9AFBD0;
using StaticLoadObjectFn = uintptr_t(__cdecl*)(uintptr_t cls, uintptr_t outer, const wchar_t* name,
                                                const wchar_t* filename, uint32_t flags, uintptr_t sandbox, bool reconcile);
constexpr char kReadyToStartMatch[] = "/Script/Engine.GameMode.ReadyToStartMatch";  // its answer is logged on change

// A call is logged one by one when its function path or its object's class name contains one of these.
static const char* kWatched[] = {
    "GameMode", "GameState", "GameSession", "WorldManager", "Mission", "AIDirector",
    "Theater", "Zone", "MatchState", "Travel", "Encounter", "Objective",
    "Matchmak", "Validate", "Lobby", "HeroSelect", "HeroChoice", "SetCurrentHero", "ServerSetHero", "SetDefaultHero",
};
// Functions whose first parameter is an FString URL worth logging (ClientTravel, SwitchLevel, ...).
static const char* kUrlFunctions[] = { "Travel", "SwitchLevel" };
// `state` counts live instances of classes whose own or parent class name contains one of these.
static const char* kCounted[] = {
    "GameMode", "GameState", "WorldManager", "MissionManager", "MissionGeneration", "AIDirector",
    "BuildingFoundation", "PlayerStart", "FortPlayerController", "FortPlayerPawn", "FortAIPawn",
};
constexpr int kCallsLoggedPerPair = 20;

using ProcessEventFn = void(__fastcall*)(uintptr_t object, void* edx, uintptr_t function, void* params);
static ProcessEventFn g_original = nullptr;

static std::wstring g_dir;
static DWORD g_gameThread = 0;
static FILE* g_seen = nullptr;
// engine::Lock() guards all of this; it is recursive, since our own commands call back into ProcessEvent.

struct NamedInfo {
    int32_t nameIndex = -1;  // to notice when a freed object's address is reused by another
    std::string name;
    bool watched = false;
    bool readyCheck = false;  // this is GameMode.ReadyToStartMatch
    bool urlParam = false;    // the name matches kUrlFunctions
    int event = 0;            // a function the mission code reacts to (game::EventFor)
    uint64_t calls = 0;
};
static std::unordered_map<uintptr_t, NamedInfo> g_functions;  // UFunction* -> path
static std::unordered_map<uintptr_t, NamedInfo> g_classes;    // UClass* -> name
static std::map<std::pair<uintptr_t, uintptr_t>, int> g_pairs;  // (class, function) -> watched calls

static bool Matches(const std::string& s, const char* const* words, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (s.find(words[i]) != std::string::npos) return true;
    return false;
}

// More words to watch, added with the `watch` command while the game runs (e.g. watch Reload).
static std::vector<std::string> g_extraWatched;

static bool Watched(const std::string& s) {
    if (Matches(s, kWatched, std::size(kWatched))) return true;
    for (const std::string& word : g_extraWatched)
        if (s.find(word) != std::string::npos) return true;
    return false;
}

// Looks up (or fills) the cache entry for an object; `full` gives the path instead of the bare name.
static NamedInfo& Lookup(std::unordered_map<uintptr_t, NamedInfo>& cache, uintptr_t object, bool full) {
    int32_t nameIndex = mem::ReadOr<int32_t>(object + ue::obj::Name, -1);
    NamedInfo& info = cache[object];
    if (info.nameIndex != nameIndex) {
        info = NamedInfo{};
        info.nameIndex = nameIndex;
        info.name = full ? ue::PathName(object) : ue::ObjectName(object);
        info.watched = Watched(info.name);
        info.readyCheck = info.name == kReadyToStartMatch;
        info.urlParam = Matches(info.name.substr(info.name.rfind('.') + 1), kUrlFunctions, std::size(kUrlFunctions));
        if (full) info.event = game::EventFor(info.name);
    }
    return info;
}

static std::string Describe(uintptr_t object) {
    return object ? ue::FullName(object) : "null";
}

static std::string ReadFString(uintptr_t addr) {
    uintptr_t data = mem::ReadOr<uint32_t>(addr);
    int32_t num = mem::ReadOr<int32_t>(addr + 4);
    std::string out;
    for (int32_t i = 0; data && i < num - 1 && i < 512; i++) {
        uint16_t c = mem::ReadOr<uint16_t>(data + i * 2);
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

// Native classes, functions and default objects live as long as the process, so engine::FindObject caches them.
using engine::FindFunction;
using engine::FindObject;
using engine::LocalPlayerController;
using engine::World;

// ---- commands -------------------------------------------------------------------------------------------------

struct FString {
    const wchar_t* data;
    int32_t num;  // characters including the terminator
    int32_t max;
};

// Calls into the engine are prepared under the lock and made by the hook after releasing it, so the engine can
// never wait on another thread that is itself waiting for our lock. They run in order; `object` is resolved when
// the call is made, so a call can use what an earlier one created. `params` is filled beforehand except the
// FStrings in `strings` (offset, characters), which point at their characters once the call is made. `after` runs
// under the lock with the parameters, to read return values. A call with `native` set runs that instead of
// ProcessEvent (for engine functions that are not UFunctions); it takes the lock itself if it needs it.
struct PendingCall {
    std::function<uintptr_t()> object;
    uintptr_t function = 0;
    std::vector<std::pair<int, std::wstring>> strings;
    alignas(4) uint8_t params[64] = {};
    std::function<void(const uint8_t* params)> after;
    std::function<void()> native;
};
static std::vector<PendingCall> g_pending;

static void Travel(const std::string& url) {
    uintptr_t switchLevel = FindFunction("/Script/Engine.PlayerController.SwitchLevel");
    uintptr_t pc = LocalPlayerController();
    if (!pc || !switchLevel) {
        LOG("travel: %s", !pc ? "no local player controller" : "PlayerController.SwitchLevel not found");
        return;
    }
    PendingCall call;
    call.object = [pc] { return pc; };
    call.function = switchLevel;
    call.strings.push_back({ 0, std::wstring(url.begin(), url.end()) });
    g_pending.push_back(std::move(call));
    LOG("travel: %s SwitchLevel(\"%s\")", Describe(pc).c_str(), url.c_str());
}

// `pc <Function> [0|1]`: calls FortPlayerController.<Function> (or PlayerController.<Function>) on the local player
// controller, with an optional bool; e.g. pc ServerReadyToStartMatch, pc ServerSetClientHasFinishedLoading 1.
static void CallOnPlayerController(const std::string& arg) {
    size_t space = arg.find(' ');
    std::string name = arg.substr(0, space);
    uintptr_t pc = LocalPlayerController();
    uintptr_t function = FindFunction("/Script/FortniteGame.FortPlayerController." + name);
    if (!function) function = FindFunction("/Script/Engine.PlayerController." + name);
    if (!pc || !function) {
        LOG("pc: %s", !pc ? "no local player controller" : ("no PlayerController function " + name).c_str());
        return;
    }
    PendingCall call;
    call.object = [pc] { return pc; };
    call.function = function;
    if (space != std::string::npos) call.params[0] = arg.substr(space + 1) == "1";
    g_pending.push_back(std::move(call));
    LOG("pc: %s %s(%s)", Describe(pc).c_str(), ue::PathName(function).c_str(),
        space != std::string::npos ? arg.substr(space + 1).c_str() : "");
}

// `console <command>`: KismetSystemLibrary.ExecuteConsoleCommand(player, command, player), as if typed into the
// game's console; e.g. console flushlog (writes the buffered engine log to FortniteGame.log).
static void Console(const std::string& command) {
    uintptr_t kismet = FindObject("/Script/Engine.Default__KismetSystemLibrary", "KismetSystemLibrary");
    uintptr_t execute = FindFunction("/Script/Engine.KismetSystemLibrary.ExecuteConsoleCommand");
    uintptr_t pc = LocalPlayerController();
    if (!kismet || !execute || !pc) {
        LOG("console: %s", !pc ? "no local player controller" : "KismetSystemLibrary.ExecuteConsoleCommand not found");
        return;
    }
    PendingCall call;
    call.object = [kismet] { return kismet; };
    call.function = execute;
    memcpy(call.params, &pc, 4);       // WorldContextObject
    call.strings.push_back({ 4, std::wstring(command.begin(), command.end()) });  // Command
    memcpy(call.params + 16, &pc, 4);  // SpecificPlayer
    g_pending.push_back(std::move(call));
    LOG("console: %s", command.c_str());
}

static uintptr_t CurrentWorldRecord() {
    uintptr_t gameState = mem::ReadOr<uint32_t>(World() + off::WorldGameState);
    uintptr_t worldManager = gameState ? mem::ReadOr<uint32_t>(gameState + off::FortGameStateWorldManager) : 0;
    return worldManager ? mem::ReadOr<uint32_t>(worldManager + off::WorldManagerCurrentWorldRecord) : 0;
}

// `missiongen <class path>`: writes the mission generator into the world record's ZoneInstanceInfo, where
// world/validate's answer would go, e.g. missiongen /Game/World/MissionGens/MissionGen_1Gate.MissionGen_1Gate_C.
// The path FString is built by the engine (KismetStringLibrary.Concat_StrStr) so the engine can free it later.
static void SetMissionGenerator(const std::string& path) {
    uintptr_t record = CurrentWorldRecord();
    uintptr_t strings = FindObject("/Script/Engine.Default__KismetStringLibrary", "KismetStringLibrary");
    uintptr_t concat = FindFunction("/Script/Engine.KismetStringLibrary.Concat_StrStr");
    if (!record || !strings || !concat) {
        LOG("missiongen: %s", !record ? "no world record (load a zone with FortGameMode first)"
                                      : "KismetStringLibrary.Concat_StrStr not found");
        return;
    }
    PendingCall call;
    call.object = [strings] { return strings; };
    call.function = concat;
    call.strings.push_back({ 0, std::wstring(path.begin(), path.end()) });  // A = path, B = empty, ReturnValue at 0x18
    call.after = [record](const uint8_t* params) {
        uintptr_t soft = record + off::RecordMissionGenerator;
        int32_t noIndex = -1, noSerial = 0, retest = -1;
        memcpy(reinterpret_cast<void*>(soft), &noIndex, 4);       // weak pointer: unresolved
        memcpy(reinterpret_cast<void*>(soft + 4), &noSerial, 4);
        memcpy(reinterpret_cast<void*>(soft + 8), &retest, 4);    // TagAtLastTest: force a lookup by path
        memcpy(reinterpret_cast<void*>(soft + off::SoftPtrPath), params + 0x18, 12);
        LOG("missiongen: record now has MissionGenerator \"%s\"", ReadFString(soft + off::SoftPtrPath).c_str());
    };
    g_pending.push_back(std::move(call));
    LOG("missiongen: %s", path.c_str());
}

// `missiongen-next <class path>`: like missiongen, but for the next zone: the engine-made FString is prepared now
// and written into the world record by the hook on the first engine call after the record exists, so it is in
// place before the world manager builds the zone.
static struct { bool armed = false; uint8_t path[12] = {}; uintptr_t lastRecord = 0; } g_nextGenerator;

static void ArmMissionGenerator(const std::string& path) {
    uintptr_t strings = FindObject("/Script/Engine.Default__KismetStringLibrary", "KismetStringLibrary");
    uintptr_t concat = FindFunction("/Script/Engine.KismetStringLibrary.Concat_StrStr");
    if (!strings || !concat) {
        LOG("missiongen-next: KismetStringLibrary.Concat_StrStr not found");
        return;
    }
    PendingCall call;
    call.object = [strings] { return strings; };
    call.function = concat;
    call.strings.push_back({ 0, std::wstring(path.begin(), path.end()) });
    call.after = [](const uint8_t* params) {
        memcpy(g_nextGenerator.path, params + 0x18, 12);
        g_nextGenerator.lastRecord = CurrentWorldRecord();  // not this world's record
        g_nextGenerator.armed = true;
        LOG("missiongen-next: armed \"%s\"", ReadFString(reinterpret_cast<uintptr_t>(g_nextGenerator.path)).c_str());
    };
    g_pending.push_back(std::move(call));
}

// Called on every game-thread engine call while armed.
static void ApplyArmedGenerator() {
    uintptr_t record = CurrentWorldRecord();
    if (!record || record == g_nextGenerator.lastRecord) return;
    uintptr_t soft = record + off::RecordMissionGenerator;
    int32_t noIndex = -1, noSerial = 0, retest = -1;
    memcpy(reinterpret_cast<void*>(soft), &noIndex, 4);
    memcpy(reinterpret_cast<void*>(soft + 4), &noSerial, 4);
    memcpy(reinterpret_cast<void*>(soft + 8), &retest, 4);
    memcpy(reinterpret_cast<void*>(soft + off::SoftPtrPath), g_nextGenerator.path, 12);
    g_nextGenerator.armed = false;
    uintptr_t gameState = mem::ReadOr<uint32_t>(World() + off::WorldGameState);
    uintptr_t worldManager = mem::ReadOr<uint32_t>(gameState + off::FortGameStateWorldManager);
    LOG("missiongen-next: written into %s (WorldManagerState %d)", Describe(record).c_str(),
        mem::ReadOr<uint8_t>(worldManager + off::WorldManagerState));
}

// `cheat <Function> [text]`: calls FortCheatManager.<Function> (or CheatManager.<Function>) with no parameter or
// one FString, e.g. cheat ListMissionGenerators, cheat LoadMissionGenerator MissionGen_1Gate. Shipping builds
// don't create a cheat manager, so one is spawned for the local player first (GameplayStatics.SpawnObject).
static void Cheat(const std::string& arg) {
    size_t space = arg.find(' ');
    std::string name = arg.substr(0, space);
    uintptr_t pc = LocalPlayerController();
    uintptr_t function = FindFunction("/Script/FortniteGame.FortCheatManager." + name);
    if (!function) function = FindFunction("/Script/Engine.CheatManager." + name);
    if (!pc || !function) {
        LOG("cheat: %s", !pc ? "no local player controller" : ("no CheatManager function " + name).c_str());
        return;
    }

    if (!mem::ReadOr<uint32_t>(pc + off::PlayerControllerCheatManager)) {
        uintptr_t statics = FindObject("/Script/Engine.Default__GameplayStatics", "GameplayStatics");
        uintptr_t spawnObject = FindFunction("/Script/Engine.GameplayStatics.SpawnObject");
        uintptr_t cheatClass = FindObject("/Script/FortniteGame.FortCheatManager", "Class");
        if (!statics || !spawnObject || !cheatClass) {
            LOG("cheat: can't create a cheat manager (GameplayStatics.SpawnObject or FortCheatManager not found)");
            return;
        }
        PendingCall spawn;
        spawn.object = [statics] { return statics; };
        spawn.function = spawnObject;
        memcpy(spawn.params, &cheatClass, 4);  // ObjectClass, Outer, ReturnValue
        memcpy(spawn.params + 4, &pc, 4);
        spawn.after = [pc](const uint8_t* params) {
            uint32_t manager;
            memcpy(&manager, params + 8, 4);
            *reinterpret_cast<uint32_t*>(pc + off::PlayerControllerCheatManager) = manager;
            LOG("cheat: created %s", Describe(manager).c_str());
        };
        g_pending.push_back(std::move(spawn));
    }

    PendingCall call;
    call.object = [pc] { return static_cast<uintptr_t>(mem::ReadOr<uint32_t>(pc + off::PlayerControllerCheatManager)); };
    call.function = function;
    if (space != std::string::npos) {
        std::string text = arg.substr(space + 1);
        call.strings.push_back({ 0, std::wstring(text.begin(), text.end()) });
    }
    g_pending.push_back(std::move(call));
    LOG("cheat: %s(%s)", ue::PathName(function).c_str(), space != std::string::npos ? arg.substr(space + 1).c_str() : "");
}

static void LogState() {
    ue::Refresh();
    uintptr_t world = World();
    LOG("state:");
    LOG("  world      %s", Describe(world).c_str());

    uintptr_t gameMode = mem::ReadOr<uint32_t>(world + off::WorldAuthorityGameMode);
    LOG("  game mode  %s", Describe(gameMode).c_str());
    if (gameMode) {
        LOG("    MatchState            %s", ue::NameOf(mem::ReadOr<int32_t>(gameMode + off::GameModeMatchState, -1)).c_str());
        if (ue::IsA(gameMode, "FortGameMode")) {
            uint8_t bits = mem::ReadOr<uint8_t>(gameMode + off::FortGameModeReadyBits);
            LOG("    bWorldIsReady %d, bTheaterDataIsReady %d, PendingTimerState %d", bits & 1, (bits >> 1) & 1,
                mem::ReadOr<uint8_t>(gameMode + off::FortGameModePendingTimerState));
            LOG("    MissionManagerClass   %s",
                Describe(mem::ReadOr<uint32_t>(gameMode + off::FortGameModeMissionManagerClass)).c_str());
            LOG("    CurrentZoneInstanceId \"%s\"", ReadFString(gameMode + off::FortGameModeZoneInstanceId).c_str());
        }
    }

    uintptr_t gameState = mem::ReadOr<uint32_t>(world + off::WorldGameState);
    LOG("  game state %s", Describe(gameState).c_str());
    if (gameState && ue::IsA(gameState, "FortGameState")) {
        LOG("    WorldLevel %d, GameplayState %d", mem::ReadOr<int32_t>(gameState + off::FortGameStateWorldLevel),
            mem::ReadOr<uint8_t>(gameState + off::FortGameStateGameplayState));
        uintptr_t missionManager = mem::ReadOr<uint32_t>(gameState + off::FortGameStateMissionManager);
        LOG("    MissionManager %s", Describe(missionManager).c_str());
        if (missionManager) {
            uintptr_t missions = mem::ReadOr<uint32_t>(missionManager + off::MissionManagerMissions);
            int32_t count = mem::ReadOr<int32_t>(missionManager + off::MissionManagerMissions + 4);
            LOG("      Missions: %d", count);
            for (int32_t i = 0; missions && i < count && i < 16; i++)
                LOG("        %s", Describe(mem::ReadOr<uint32_t>(missions + i * 4)).c_str());
        }
        uintptr_t worldManager = mem::ReadOr<uint32_t>(gameState + off::FortGameStateWorldManager);
        LOG("    WorldManager   %s", Describe(worldManager).c_str());
        uintptr_t record = worldManager ? mem::ReadOr<uint32_t>(worldManager + off::WorldManagerCurrentWorldRecord) : 0;
        LOG("      CurrentWorldRecord %s", Describe(record).c_str());
        if (record) {
            LOG("        WorldId \"%s\", TheaterId \"%s\", TheaterMissionId \"%s\", TileIndex %d",
                ReadFString(record + off::RecordWorldId).c_str(), ReadFString(record + off::RecordTheaterId).c_str(),
                ReadFString(record + off::RecordTheaterMissionId).c_str(), mem::ReadOr<int32_t>(record + off::RecordTileIndex));
            LOG("        ZoneThemeClass   \"%s\"", ReadFString(record + off::RecordZoneThemeClass + off::SoftPtrPath).c_str());
            LOG("        MissionGenerator \"%s\"", ReadFString(record + off::RecordMissionGenerator + off::SoftPtrPath).c_str());
        }
    }
    uintptr_t pc = LocalPlayerController();
    LOG("  player     %s", Describe(pc).c_str());
    if (pc) LOG("    CheatManager %s", Describe(mem::ReadOr<uint32_t>(pc + off::PlayerControllerCheatManager)).c_str());
    if (pc && ue::IsA(pc, "FortPlayerController")) {
        uint8_t bits = mem::ReadOr<uint8_t>(pc + off::FortPlayerControllerSpawnBits);
        LOG("    bHasInitiallySpawned %d, bReadyToStartMatch %d, bClientPawnIsLoaded %d, "
            "bHasClientFinishedLoading %d, bHasServerFinishedLoading %d",
            (bits >> 3) & 1, (bits >> 5) & 1, (bits >> 6) & 1,
            mem::ReadOr<uint8_t>(pc + off::FortPlayerControllerClientLoaded),
            mem::ReadOr<uint8_t>(pc + off::FortPlayerControllerServerLoaded));
    }

    // Live instances (default objects skipped) of the classes in kCounted, by class.
    std::unordered_map<uintptr_t, bool> counted;  // class -> its own or a parent's name is in kCounted
    std::map<std::string, int> counts;
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (!object) continue;
        uintptr_t cls = mem::ReadOr<uint32_t>(object + ue::obj::Class);
        auto it = counted.find(cls);
        if (it == counted.end()) {
            bool match = false;
            for (uintptr_t c = cls; c && !match; c = mem::ReadOr<uint32_t>(c + ue::strct::Super))
                match = Matches(ue::ObjectName(c), kCounted, std::size(kCounted));
            it = counted.emplace(cls, match).first;
        }
        if (!it->second || ue::ObjectName(object).rfind("Default__", 0) == 0) continue;
        counts[ue::ObjectName(cls)]++;
    }
    LOG("  live mission-related objects:");
    if (counts.empty()) LOG("    (none)");
    for (auto& kv : counts) LOG("    %5d x %s", kv.second, kv.first.c_str());
}

static void WriteSummary() {
    FILE* f = _wfopen((g_dir + L"summary.txt").c_str(), L"w");
    if (!f) return;
    std::vector<const NamedInfo*> sorted;
    for (auto& kv : g_functions) sorted.push_back(&kv.second);
    std::sort(sorted.begin(), sorted.end(), [](const NamedInfo* a, const NamedInfo* b) { return a->calls > b->calls; });
    fprintf(f, "calls\tfunction (all ProcessEvent calls since the hook went in)\n");
    for (const NamedInfo* info : sorted) fprintf(f, "%llu\t%s\n", info->calls, info->name.c_str());
    fprintf(f, "\nwatched calls\tclass -> function\n");
    for (auto& kv : g_pairs) {
        auto cls = g_classes.find(kv.first.first);
        auto fn = g_functions.find(kv.first.second);
        fprintf(f, "%d\t%s -> %s\n", kv.second, cls != g_classes.end() ? cls->second.name.c_str() : "?",
                fn != g_functions.end() ? fn->second.name.c_str() : "?");
    }
    fclose(f);
    LOG("summary.txt: %zu functions", sorted.size());
}

// `natives`: writes re\natives.txt (every UFunction: native entry as exe RVA, FunctionFlags, path) and
// re\classes.txt (every class: its default object's vtable as exe RVA, size, parent, path), so the exe can be
// read statically with names (tools\re.js). UFunction::FunctionFlags 0x58, ExecFunction 0x74;
// UClass::ClassDefaultObject 0xA8; UStruct::PropertiesSize 0x28 (Blaze's SDK).
static void DumpNatives() {
    ue::Refresh();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(ue::g.exeBase);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(ue::g.exeBase + dos->e_lfanew);
    uintptr_t end = ue::g.exeBase + nt->OptionalHeader.SizeOfImage;
    auto rva = [end](uintptr_t addr) -> uint32_t {
        return addr >= ue::g.exeBase && addr < end ? static_cast<uint32_t>(addr - ue::g.exeBase) : 0;
    };

    std::wstring dir = g_dir + L"re\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* functions = _wfopen((dir + L"natives.txt").c_str(), L"w");
    FILE* classes = _wfopen((dir + L"classes.txt").c_str(), L"w");
    if (!functions || !classes) return;
    fprintf(functions, "# exec RVA\tFunctionFlags\tpath  (RVA 0 = not in the exe; image base in the file is 0x400000)\n");
    fprintf(classes, "# vtable RVA\tsize\tparent\tpath\n");
    int nf = 0, nc = 0;
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (!object) continue;
        std::string cls = ue::ClassName(object);
        if (cls == "Function") {
            fprintf(functions, "%08X\t%08X\t%s\n", rva(mem::ReadOr<uint32_t>(object + 0x74)),
                    mem::ReadOr<uint32_t>(object + 0x58), ue::PathName(object).c_str());
            nf++;
        } else if (cls == "Class" || cls == "BlueprintGeneratedClass") {
            uintptr_t cdo = mem::ReadOr<uint32_t>(object + 0xA8);
            uintptr_t super = mem::ReadOr<uint32_t>(object + ue::strct::Super);
            fprintf(classes, "%08X\t%X\t%s\t%s\n", rva(cdo ? mem::ReadOr<uint32_t>(cdo) : 0),
                    mem::ReadOr<int32_t>(object + 0x28), super ? ue::ObjectName(super).c_str() : "-",
                    ue::PathName(object).c_str());
            nc++;
        }
    }
    fclose(functions);
    fclose(classes);
    LOG("natives: re\\natives.txt %d functions, re\\classes.txt %d classes", nf, nc);
}

// ---- generic commands: load, call, set, get, actors ---------------------------------------------------------------

static uintptr_t FindObjectByPath(const std::string& path) {
    ue::Refresh();
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (object && ue::PathName(object) == path) return object;
    }
    return 0;
}

// Objects in commands: gm, gs, pc, wm (world manager), mm (mission manager), world, 0x<address>, or a path as
// PathName writes it (Package.Outer.Name), e.g. /Script/FortniteGame.Default__FortMissionLibrary.
static uintptr_t ResolveObject(const std::string& spec) {
    uintptr_t world = World();
    uintptr_t gameState = mem::ReadOr<uint32_t>(world + off::WorldGameState);
    if (spec == "world") return world;
    if (spec == "gm") return mem::ReadOr<uint32_t>(world + off::WorldAuthorityGameMode);
    if (spec == "gs") return gameState;
    if (spec == "pc") return LocalPlayerController();
    if (spec == "wm") return gameState ? mem::ReadOr<uint32_t>(gameState + off::FortGameStateWorldManager) : 0;
    if (spec == "mm") return gameState ? mem::ReadOr<uint32_t>(gameState + off::FortGameStateMissionManager) : 0;
    if (spec.rfind("0x", 0) == 0) return static_cast<uintptr_t>(strtoul(spec.c_str() + 2, nullptr, 16));
    return FindObjectByPath(spec);
}

static std::vector<std::string> Words(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(' ', pos);
        if (end == std::string::npos) end = text.size();
        if (end > pos) out.push_back(text.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

// Typed values: o:<object> (or o:null), i:<int>, f:<float>, b:<0|1>, s:<text without spaces>. Writes the value at
// `at` (s: is recorded in `strings` instead) and returns its size, or 0 for a bad value.
static int PutValue(const std::string& typed, uint8_t* at, int offset, std::vector<std::pair<int, std::wstring>>* strings) {
    if (typed.size() < 2 || typed[1] != ':') return 0;
    std::string v = typed.substr(2);
    switch (typed[0]) {
    case 'o': {
        uint32_t o = v == "null" ? 0 : static_cast<uint32_t>(ResolveObject(v));
        if (!o && v != "null") return 0;
        memcpy(at, &o, 4);
        return 4;
    }
    case 'i': { int32_t x = static_cast<int32_t>(strtol(v.c_str(), nullptr, 0)); memcpy(at, &x, 4); return 4; }
    case 'f': { float x = strtof(v.c_str(), nullptr); memcpy(at, &x, 4); return 4; }
    case 'b': *at = v == "1"; return 1;
    case 's':
        if (!strings) return 0;
        strings->push_back({ offset, std::wstring(v.begin(), v.end()) });
        return 12;
    }
    return 0;
}

// `call <object> <function path> [typed args]`: ProcessEvent with the arguments packed in order the way UE lays
// out parameters (4-byte aligned, bools 1 byte); logs the first 32 bytes of the parameter block afterwards, where
// return values and out parameters land. E.g. call /Script/FortniteGame.Default__FortMissionLibrary
// /Script/FortniteGame.FortMissionLibrary.LoadMission o:gm o:/Game/Blueprints/Adventures/Gate/GateWithBluGlo.GateWithBluGlo
static void CallFunction(const std::vector<std::string>& words) {
    if (words.size() < 2) { LOG("call <object> <function path> [o:|i:|f:|b:|s: args]"); return; }
    uintptr_t object = ResolveObject(words[0]);
    uintptr_t function = FindFunction(words[1]);
    if (!object || !function) {
        LOG("call: %s", !object ? ("no object " + words[0]).c_str() : ("no function " + words[1]).c_str());
        return;
    }
    PendingCall call;
    call.object = [object] { return object; };
    call.function = function;
    int at = 0;
    for (size_t i = 2; i < words.size(); i++) {
        if (words[i][0] != 'b') at = (at + 3) & ~3;
        if (at + 12 > static_cast<int>(sizeof(call.params))) { LOG("call: too many arguments"); return; }
        int size = PutValue(words[i], call.params + at, at, &call.strings);
        if (!size) { LOG("call: bad argument %s", words[i].c_str()); return; }
        at += size;
    }
    std::string label = words[1];
    call.after = [label](const uint8_t* params) {
        char bytes[3 * 32 + 1] = {};
        for (int i = 0; i < 32; i++) snprintf(bytes + i * 3, 4, "%02X ", params[i]);
        LOG("call: %s done; params %s", label.c_str(), bytes);
    };
    g_pending.push_back(std::move(call));
    LOG("call: %s on %s", words[1].c_str(), Describe(object).c_str());
}

// `load <object path>`: StaticLoadObject(UObject class, null, path), for assets nothing has loaded yet, e.g.
// load /Game/Blueprints/Adventures/Gate/GateWithBluGlo.GateWithBluGlo
static void Load(const std::string& path) {
    uintptr_t objectClass = FindObject("/Script/CoreUObject.Object", "Class");
    if (!objectClass) { LOG("load: the UObject class was not found"); return; }
    PendingCall call;
    std::wstring wide(path.begin(), path.end());
    call.native = [objectClass, wide, path] {
        auto load = reinterpret_cast<StaticLoadObjectFn>(ue::g.exeBase + kStaticLoadObject);
        uintptr_t object = load(objectClass, 0, wide.c_str(), nullptr, 0, 0, true);
        std::lock_guard<std::recursive_mutex> guard(engine::Lock());
        LOG("load: %s -> %s", path.c_str(), Describe(object).c_str());
    };
    g_pending.push_back(std::move(call));
    LOG("load: %s", path.c_str());
}

// `set <object> <offset> <typed value>` writes one field, e.g. the Zone classes into FortGameMode's defaults:
// set /Script/FortniteGame.Default__FortGameMode 0x29C o:/Script/FortniteGame.FortGameStateZone
// (AGameMode PlayerControllerClass 0x28C, PlayerStateClass 0x298, GameStateClass 0x29C).
// `get <object> <offset> [dwords]` reads.
static void SetField(const std::vector<std::string>& w) {
    if (w.size() != 3) { LOG("set <object> <offset> <o:|i:|f:|b: value>"); return; }
    uintptr_t object = ResolveObject(w[0]);
    int offset = static_cast<int>(strtol(w[1].c_str(), nullptr, 0));
    uint8_t value[12] = {};
    int size = object ? PutValue(w[2], value, 0, nullptr) : 0;
    if (!size || !mem::Readable(object + offset, size)) { LOG("set: bad object or value (%s)", w[0].c_str()); return; }
    uint32_t before = mem::ReadOr<uint32_t>(object + offset);
    memcpy(reinterpret_cast<void*>(object + offset), value, size);
    LOG("set: %s +0x%X: %08X -> %08X", Describe(object).c_str(), offset, before, mem::ReadOr<uint32_t>(object + offset));
}

static void GetField(const std::vector<std::string>& w) {
    if (w.size() < 2) { LOG("get <object> <offset> [dwords]"); return; }
    uintptr_t object = ResolveObject(w[0]);
    int offset = static_cast<int>(strtol(w[1].c_str(), nullptr, 0));
    int count = w.size() > 2 ? atoi(w[2].c_str()) : 1;
    if (!object) { LOG("get: no object %s", w[0].c_str()); return; }
    for (int i = 0; i < count && i < 64; i++) {
        uint32_t v = mem::ReadOr<uint32_t>(object + offset + i * 4);
        float f;
        memcpy(&f, &v, 4);
        std::string asObject = v >= 0x10000 && mem::Readable(v, 0x1C) && mem::Readable(mem::ReadOr<uint32_t>(v + ue::obj::Class), 0x1C)
            ? ue::FullName(v) : "";
        LOG("get: %s +0x%X = %08X  (%d, %g) %s", Describe(object).c_str(), offset + i * 4, v, static_cast<int32_t>(v), f,
            asObject.c_str());
    }
}

// `props <object> [name part]`: the object's UPROPERTYs, its class's and its supers' (UStruct::Children +0x24,
// UField::Next +0x1C, UProperty::Offset_Internal +0x3C), with their offsets and values: objects by name, numbers,
// strings, names, array lengths; bools as their whole byte.
static void ListProperties(const std::vector<std::string>& w) {
    if (w.empty()) { LOG("props <object> [name part]"); return; }
    uintptr_t object = ResolveObject(w[0]);
    if (!object) { LOG("props: no object %s", w[0].c_str()); return; }
    std::string part = w.size() > 1 ? w[1] : "";
    int shown = 0;
    for (uintptr_t c = mem::ReadOr<uint32_t>(object + ue::obj::Class); c && shown < 150; c = mem::ReadOr<uint32_t>(c + ue::strct::Super)) {
        for (uintptr_t field = mem::ReadOr<uint32_t>(c + 0x24); field && shown < 150; field = mem::ReadOr<uint32_t>(field + 0x1C)) {
            std::string type = ue::ClassName(field), name = ue::ObjectName(field);
            if (type.size() < 8 || type.compare(type.size() - 8, 8, "Property") != 0) continue;
            if (!part.empty() && name.find(part) == std::string::npos) continue;
            int offset = mem::ReadOr<int32_t>(field + 0x3C);
            uintptr_t at = object + offset;
            uint32_t v = mem::ReadOr<uint32_t>(at);
            std::string value;
            char buffer[64];
            if (type == "ObjectProperty" || type == "ClassProperty" || type == "WeakObjectProperty" || type == "InterfaceProperty") {
                value = v >= 0x10000 && mem::Readable(v, 0x1C) ? ue::FullName(v) : (v ? "?" : "null");
            } else if (type == "IntProperty" || type == "UInt32Property") {
                snprintf(buffer, sizeof(buffer), "%d", static_cast<int32_t>(v)); value = buffer;
            } else if (type == "FloatProperty") {
                float f; memcpy(&f, &v, 4); snprintf(buffer, sizeof(buffer), "%g", f); value = buffer;
            } else if (type == "ByteProperty") {
                snprintf(buffer, sizeof(buffer), "%u", v & 0xFF); value = buffer;
            } else if (type == "BoolProperty") {
                snprintf(buffer, sizeof(buffer), "byte %02X", v & 0xFF); value = buffer;  // the bit's mask isn't read here
            } else if (type == "NameProperty") {
                value = ue::NameOf(static_cast<int32_t>(v));
            } else if (type == "StrProperty" || type == "TextProperty") {
                uintptr_t data = type == "StrProperty" ? v : 0;
                int32_t num = mem::ReadOr<int32_t>(at + 4);
                for (int32_t i = 0; data && i < num - 1 && i < 80; i++) value.push_back(static_cast<char>(mem::ReadOr<uint16_t>(data + i * 2)));
                value = type == "TextProperty" ? "(text)" : "\"" + value + "\"";
            } else if (type == "ArrayProperty") {
                snprintf(buffer, sizeof(buffer), "%d element(s) at %08X", mem::ReadOr<int32_t>(at + 4), v); value = buffer;
            } else if (type == "StructProperty") {
                uintptr_t st = mem::ReadOr<uint32_t>(field + 0x44);
                snprintf(buffer, sizeof(buffer), "{%08X %08X %08X}", v, mem::ReadOr<uint32_t>(at + 4), mem::ReadOr<uint32_t>(at + 8));
                value = (st ? ue::ObjectName(st) : std::string("struct")) + " " + buffer;
            } else {
                snprintf(buffer, sizeof(buffer), "%08X", v); value = buffer;
            }
            LOG("  +0x%03X %s %s = %s", offset, type.c_str(), name.c_str(), value.c_str());
            shown++;
        }
    }
    LOG("props: %d of %s's properties shown", shown, Describe(object).c_str());
}

// `funcs <object>`: the UFunctions of its class and supers up to the first native class (a blueprint's events).
static void ListFunctions(const std::string& spec) {
    uintptr_t object = ResolveObject(spec);
    if (!object) { LOG("funcs: no object %s", spec.c_str()); return; }
    int shown = 0;
    for (uintptr_t c = mem::ReadOr<uint32_t>(object + ue::obj::Class); c && shown < 200; c = mem::ReadOr<uint32_t>(c + ue::strct::Super)) {
        for (uintptr_t field = mem::ReadOr<uint32_t>(c + 0x24); field && shown < 200; field = mem::ReadOr<uint32_t>(field + 0x1C)) {
            if (ue::ClassName(field) != "Function") continue;
            LOG("  %s.%s", ue::ObjectName(c).c_str(), ue::ObjectName(field).c_str());
            shown++;
        }
        if (ue::ObjectName(c).find("_C") == std::string::npos) break;  // stop after the first native class
    }
    LOG("funcs: %d function(s) of %s", shown, Describe(object).c_str());
}

// `params <function path>`: a UFunction's parameters (its own UProperty children) with their offsets and types.
static void ListParams(const std::string& path) {
    uintptr_t function = engine::FindFunction(path);
    if (!function) { LOG("params: no function %s", path.c_str()); return; }
    for (uintptr_t field = mem::ReadOr<uint32_t>(function + 0x24); field; field = mem::ReadOr<uint32_t>(field + 0x1C))
        LOG("  +0x%03X %s %s (size %d)", mem::ReadOr<int32_t>(field + 0x3C), ue::ClassName(field).c_str(), ue::ObjectName(field).c_str(),
            mem::ReadOr<int32_t>(field + 0x24));  // UProperty::ElementSize
    LOG("params: %s", path.c_str());
}

// `actors <class name part> [near]`: live objects whose class or a parent class has this in its name, with the root
// component's location (AActor::RootComponent 0x114, USceneComponent::RelativeLocation 0x120); with "near", the 20
// nearest to this player's pawn. `face <class name part>`: this player turns to the nearest one (test swings and shots).
// `goto <class name part | actor name>`: this player's pawn next to the nearest one (several spots around it, a
// little above, until a teleport takes).
static void ListActors(std::string part, bool face = false, bool go = false) {
    ue::Refresh();
    bool nearest = face || go || (part.size() > 5 && part.compare(part.size() - 5, 5, " near") == 0);
    if (nearest) {
        if (!face && !go) part.resize(part.size() - 5);
        float from[3] = {};
        engine::Location(engine::PawnOf(engine::LocalPlayerController()), from);
        std::vector<std::pair<float, uintptr_t>> found;
        for (int32_t i = 0; i < ue::g.numObjects; i++) {
            uintptr_t object = ue::ObjectAt(i);
            if (!object || !engine::IsLive(object) || ue::ObjectName(object).rfind("Default__", 0) == 0) continue;
            bool match = ue::ObjectName(object) == part;  // an actor by its own name, or by its class
            for (uintptr_t c = mem::ReadOr<uint32_t>(object + ue::obj::Class); c && !match; c = mem::ReadOr<uint32_t>(c + ue::strct::Super))
                match = ue::ObjectName(c).find(part) != std::string::npos;
            uintptr_t root = match ? mem::ReadOr<uint32_t>(object + off::ActorRootComponent) : 0;
            if (!root) continue;
            float d2 = 0;
            for (int k = 0; k < 3; k++) {
                float d = mem::ReadOr<float>(root + off::SceneComponentRelativeLocation + k * 4) - from[k];
                d2 += d * d;
            }
            if (d2 >= 0) found.emplace_back(d2, object);  // not NaN (a non-actor read as one)
        }
        std::sort(found.begin(), found.end());
        if (go && part == "goal" && atlas::Goal()) found.insert(found.begin(), { 0.0f, atlas::Goal() });  // the mission's first prop
        if (go) {
            uintptr_t pawn = engine::PawnOf(engine::LocalPlayerController());
            float to[3] = {};
            if (found.empty() || !pawn || !engine::Location(found[0].second, to)) {
                LOG("goto: nothing like \"%s\" (or no pawn)", part.c_str());
                return;
            }
            // Beside it on the navmesh (an actor's origin can be under the ground there), a little above.
            const float kSpots[][2] = { { -400, 0 }, { 400, 0 }, { 0, -400 }, { 0, 400 }, { -700, -700 }, { 700, 700 } };
            uintptr_t project = engine::FindFunction("/Script/Engine.NavigationSystem.ProjectPointToNavigation");
            uintptr_t navDefaults = engine::FindObject("/Script/Engine.Default__NavigationSystem", "NavigationSystem");
            for (const auto& spot : kSpots) {
                struct {
                    uintptr_t context;
                    float point[3];
                    uintptr_t navData, filter;
                    float extent[3];
                    float result[3];
                } p{ engine::GameMode(), { to[0] + spot[0], to[1] + spot[1], to[2] }, 0, 0, { 300, 300, 5000 }, {} };
                if (!project || !navDefaults) break;
                engine::ProcessEvent(navDefaults, project, &p);
                if (p.result[0] == p.point[0] && p.result[1] == p.point[1] && p.result[2] == p.point[2]) continue;  // no navmesh
                float at[3] = { p.result[0], p.result[1], p.result[2] + 150.0f };
                if (engine::Teleport(pawn, at)) {
                    LOG("goto: %s at (%.0f, %.0f, %.0f); the pawn is at (%.0f, %.0f, %.0f)", ue::FullName(found[0].second).c_str(), to[0],
                        to[1], to[2], at[0], at[1], at[2]);
                    return;
                }
            }
            LOG("goto: no free spot by %s", ue::FullName(found[0].second).c_str());
            return;
        }
        if (face) {
            uintptr_t pc = engine::LocalPlayerController();
            float to[3] = {};
            if (found.empty() || !pc || !engine::Location(found[0].second, to)) {
                LOG("face: nothing like \"%s\" to face", part.c_str());
                return;
            }
            float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] + 80.0f - from[2];
            struct { float pitch, yaw, roll; } rotation = {
                static_cast<float>(std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.29578), static_cast<float>(std::atan2(dy, dx) * 57.29578), 0 };
            engine::ProcessEvent(pc, engine::FindFunction("/Script/Engine.Controller.SetControlRotation"), &rotation);
            LOG("face: %s, %.0f away (yaw %.0f, pitch %.0f)", ue::FullName(found[0].second).c_str(), std::sqrt(found[0].first),
                rotation.yaw, rotation.pitch);
            return;
        }
        for (size_t i = 0; i < found.size() && i < 20; i++)
            LOG("  %s  %.0f away", ue::FullName(found[i].second).c_str(), std::sqrt(found[i].first));
        LOG("actors: %zu match \"%s\" (the 20 nearest shown)", found.size(), part.c_str());
        return;
    }
    std::unordered_map<uintptr_t, bool> matching;  // class -> matches
    int total = 0, shown = 0;
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (!object) continue;
        uintptr_t cls = mem::ReadOr<uint32_t>(object + ue::obj::Class);
        auto it = matching.find(cls);
        if (it == matching.end()) {
            bool match = false;
            for (uintptr_t c = cls; c && !match; c = mem::ReadOr<uint32_t>(c + ue::strct::Super))
                match = ue::ObjectName(c).find(part) != std::string::npos;
            it = matching.emplace(cls, match).first;
        }
        if (!it->second || ue::ObjectName(object).rfind("Default__", 0) == 0) continue;
        total++;
        if (shown >= 60) continue;
        uintptr_t root = mem::ReadOr<uint32_t>(object + off::ActorRootComponent);
        float xyz[3] = {};
        if (root) for (int k = 0; k < 3; k++) xyz[k] = mem::ReadOr<float>(root + off::SceneComponentRelativeLocation + k * 4);
        LOG("  %s  at (%.0f, %.0f, %.0f)", ue::FullName(object).c_str(), xyz[0], xyz[1], xyz[2]);
        shown++;
    }
    LOG("actors: %d match \"%s\"%s", total, part.c_str(), total > shown ? " (first 60 shown)" : "");
}

static void RunCommand(const std::string& line) {
    LOG("> %s", line.c_str());
    size_t space = line.find(' ');
    std::string cmd = line.substr(0, space);
    std::string arg = space == std::string::npos ? "" : line.substr(space + 1);
    if (game::Command(cmd, arg)) return;
    if (cmd == "travel" && !arg.empty()) Travel(arg);
    else if (cmd == "watch") {
        // Watch more calls from now on: functions or classes whose name contains the word (no word: list them).
        if (!arg.empty()) {
            g_extraWatched.push_back(arg);
            g_functions.clear();  // the cached decisions are made again
            g_classes.clear();
            g_pairs.clear();
        }
        std::string all;
        for (const std::string& word : g_extraWatched) all += " " + word;
        LOG("watch: also watching%s", all.empty() ? " nothing more" : all.c_str());
    } else if (cmd == "pc" && !arg.empty()) CallOnPlayerController(arg);
    else if (cmd == "cheat" && !arg.empty()) Cheat(arg);
    else if (cmd == "console" && !arg.empty()) Console(arg);
    else if (cmd == "missiongen" && !arg.empty()) SetMissionGenerator(arg);
    else if (cmd == "missiongen-next" && !arg.empty()) ArmMissionGenerator(arg);
    else if (cmd == "natives") DumpNatives();
    else if (cmd == "load" && !arg.empty()) Load(arg);
    else if (cmd == "call") CallFunction(Words(arg));
    else if (cmd == "set") SetField(Words(arg));
    else if (cmd == "get") GetField(Words(arg));
    else if (cmd == "props") ListProperties(Words(arg));
    else if (cmd == "params" && !arg.empty()) ListParams(arg);
    else if (cmd == "funcs" && !arg.empty()) ListFunctions(arg);
    else if (cmd == "actors" && !arg.empty()) ListActors(arg);
    else if (cmd == "face" && !arg.empty()) ListActors(arg, true);
    else if (cmd == "goto" && !arg.empty()) ListActors(arg, false, true);
    else if (cmd == "state") LogState();
    else if (cmd == "summary") WriteSummary();
    else if (cmd == "dump") {
        std::wstring dumpDir = g_dir + L"dump\\";
        CreateDirectoryW(dumpDir.c_str(), nullptr);
        ue::Refresh();
        dump::Names(dumpDir);
        dump::Objects(dumpDir);
        LOG("mission-related classes:");
        dump::MissionClasses();
    } else LOG("unknown command; use travel <url>, pc <Function> [0|1], cheat <Function> [text], console <command>, "
               "missiongen <class path>, missiongen-next <class path>, natives, load <path>, call <object> <function> "
               "[args], set/get <object> <offset> ..., actors <class part>, state, summary, dump, or a mission "
               "command (trace.h)");
}

void Execute(const std::string& line) {
    std::lock_guard<std::recursive_mutex> guard(engine::Lock());
    RunCommand(line);
}

static void ReadCommands() {
    std::wstring path = g_dir + L"command.txt";
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return;
    std::vector<std::string> lines;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    fclose(f);
    DeleteFileW(path.c_str());
    for (auto& line : lines) RunCommand(line);
}

// Runs on the game thread at most every 500 ms: commands, world changes, scheduled state logs.
static void Poll() {
    static ULONGLONG next = 0;
    static uintptr_t lastWorld = 0;
    static std::vector<ULONGLONG> stateDue;
    static bool busy = false;
    ULONGLONG now = GetTickCount64();
    if (busy || now < next) return;
    busy = true;
    next = now + 500;

    uintptr_t world = World();
    if (world != lastWorld) {
        lastWorld = world;
        LOG("world is now %s", Describe(world).c_str());
        stateDue = { now + 3000, now + 15000 };
    }
    if (!stateDue.empty() && now >= stateDue.front()) {
        stateDue.erase(stateDue.begin());
        LogState();
    }
    ReadCommands();
    if (g_seen) fflush(g_seen);
    busy = false;
}

// ---- the hook -------------------------------------------------------------------------------------------------

struct CallInfo {
    bool readyCheck;  // GameMode.ReadyToStartMatch, whose answer the hook reads after the call
    int event;        // a call the mission code reacts to
};

static CallInfo OnCall(uintptr_t object, uintptr_t function, void* params) {
    NamedInfo& fn = Lookup(g_functions, function, true);
    CallInfo info{ fn.readyCheck, fn.event };
    if (fn.calls++ == 0 && g_seen) {
        uintptr_t cls = mem::ReadOr<uint32_t>(object + ue::obj::Class);
        fprintf(g_seen, "%s  (on %s)\n", fn.name.c_str(), ue::ObjectName(cls).c_str());
    }
    uintptr_t cls = mem::ReadOr<uint32_t>(object + ue::obj::Class);
    NamedInfo& clsInfo = Lookup(g_classes, cls, false);
    if (!fn.watched && !clsInfo.watched) return info;

    int n = ++g_pairs[{ cls, function }];
    if (n <= kCallsLoggedPerPair) {
        std::string url = fn.urlParam && params
            ? "  url \"" + ReadFString(reinterpret_cast<uintptr_t>(params)) + "\"" : "";
        LOG("call %s %s -> %s%s%s", clsInfo.name.c_str(), ue::ObjectName(object).c_str(), fn.name.c_str(), url.c_str(),
            n == kCallsLoggedPerPair ? "  (further calls of this pair only counted)" : "");
    }
    return info;
}

static void RunPending(std::vector<PendingCall>& calls) {
    for (PendingCall& call : calls) {
        if (call.native) {
            call.native();
            continue;
        }
        uintptr_t target;
        {
            std::lock_guard<std::recursive_mutex> guard(engine::Lock());
            target = call.object();
            if (!target) LOG("call skipped: no object for %s", ue::PathName(call.function).c_str());
        }
        if (!target) continue;
        for (auto& [offset, text] : call.strings) {
            int32_t num = static_cast<int32_t>(text.size() + 1);
            FString param{ text.c_str(), num, num };
            memcpy(call.params + offset, &param, sizeof(param));
        }
        g_original(target, nullptr, call.function, call.params);
        if (call.after) {
            std::lock_guard<std::recursive_mutex> guard(engine::Lock());
            call.after(call.params);
        }
    }
}

static void __fastcall ProcessEventHook(uintptr_t object, void* edx, uintptr_t function, void* params) {
    std::vector<PendingCall> calls;
    CallInfo info;
    bool gameThread = GetCurrentThreadId() == g_gameThread;
    {
        std::lock_guard<std::recursive_mutex> guard(engine::Lock());
        info = OnCall(object, function, params);
        if (gameThread) {
            if (g_nextGenerator.armed) ApplyArmedGenerator();
            Poll();
            calls.swap(g_pending);
        }
    }
    if (!calls.empty()) RunPending(calls);
    if (gameThread) {
        game::Tick();
        if (info.event) game::OnEvent(info.event, object, params);
    }
    g_original(object, edx, function, params);

    // ReadyToStartMatch's only parameter is its bool return value.
    if (info.readyCheck && params) {
        static int last = -1;
        int now = *static_cast<const uint8_t*>(params) ? 1 : 0;
        if (now != last) {
            std::lock_guard<std::recursive_mutex> guard(engine::Lock());
            LOG("ReadyToStartMatch on %s now returns %s", ue::ClassName(object).c_str(), now ? "true" : "false");
            last = now;
        }
    }
}

// The game thread is the process's first thread: the one with the earliest creation time.
static DWORD FindGameThread() {
    DWORD pid = GetCurrentProcessId(), best = 0;
    ULONGLONG bestTime = ~0ull;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{ sizeof(entry) };
    for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
        if (entry.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!t) continue;
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(t, &created, &exited, &kernel, &user)) {
            ULONGLONG time = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (time < bestTime) { bestTime = time; best = entry.th32ThreadID; }
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return best;
}

bool Start(const std::wstring& dir) {
    g_dir = dir;
    g_gameThread = FindGameThread();
    g_seen = _wfopen((dir + L"seen.log").c_str(), L"w");
    DeleteFileW((dir + L"command.txt").c_str());  // a stale command from an earlier run must not fire now
    LOG("game thread %lu", g_gameThread);

    if (!hook::Install(ue::g.exeBase + kProcessEvent, kProcessEventStart, sizeof(kProcessEventStart),
                       reinterpret_cast<void*>(&ProcessEventHook), reinterpret_cast<void**>(&g_original))) {
        LOG("ProcessEvent hook NOT installed (unexpected bytes at exe+0x%X, or patching failed)", kProcessEvent);
        return false;
    }
    engine::SetProcessEvent(reinterpret_cast<void*>(g_original));
    LOG("ProcessEvent hooked at exe+0x%X", kProcessEvent);
    return true;
}

}  // namespace trace
