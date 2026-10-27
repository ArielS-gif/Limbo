#include "backend.h"
#include "engine.h"
#include "log.h"
#include "mission.h"
#include "trace.h"
#include <windows.h>
#include <cstring>
#include <mutex>

namespace lobby {

using engine::At;

namespace {
// The Play tab: FortMatchmakingContext.StartMatchmaking would search for a server for the selected mission, and none
// ever answers ("No Matches Available"; offline, the party updates around it fail too). Its UFunction's native pointer
// (+0x74) is wrapped: the mission selected on the theater map is read here, the launch screen shown (below), and the
// zone opened with that mission's generator. The game's own search runs only when no mission can be read.
constexpr uint32_t kStartMatchmakingExec = 0x00A5CA20;
// Quick Match (the hex in the middle of the map) starts with FortMatchmakingContext.StartCriticalMission(
// MinMissionPower, MaxMissionPower) instead, a search for any mission in that threat range. Its exec thunk reads the
// two ints and calls the implementation (thiscall, ret 8) from 00A5C54C; that call is pointed here.
constexpr uint32_t kStartCriticalMissionCall = 0x00A5C54C, kStartCriticalMission = 0x007F0880;
// Start Mission is greyed out ("Cannot Start Mission until the entire party is in Homebase") when
// FortPartyContext.IsEntirePartyInFrontEnd says no; offline, after a few trips to zones, the local member's party state
// can stay on its zone. Wrapped the same way: in the frontend, a party of one is in Homebase.
constexpr uint32_t kIsEntirePartyInFrontEndExec = 0x00A3F0F0;
constexpr int FunctionNative = 0x74, FrameCode = 0x10;  // UFunction; FFrame.Code
constexpr int ClassDefaultObject = 0xA8, ObjectClass = 0xC;
// AFortTheaterMapViewer: CurrentTheaterIndex, CurrentSelectedTile, TheaterList (FFortTheaterMapData, 0xD0 each, its
// UniqueId FString at +0xC). AFortTheaterMapTile: ZoneThemeClass, MissionData (FFortAvailableMissionData: MissionGuid
// FString, MissionGenerator soft class pointer at +0x30).
constexpr int ViewerTheaterIndex = 0x294, ViewerSelectedTile = 0x2A8, ViewerTheaters = 0x2AC;
constexpr int TheaterSize = 0xD0, TheaterUniqueId = 0xC;
constexpr int TileZoneTheme = 0x254, TileMissionData = 0x2A4, MissionGeneratorRef = 0x30;
constexpr int SoftPath = 0xC;  // TSoftObjectPtr/TSoftClassPtr: the path FString after the weak pointer and its tag
constexpr int GeneratorMissionInfo = 0x424, GeneratorPrimaryMissionInfo = 0x40;  // UFortMissionGenerator (soft pointers)

// A zone theme's map in this client: the Temperate zones, the onboarding fort (the first mission, "help Ramirez") and
// the four outposts. `playmission` names them by the short name.
const struct {
    const char* theme;
    const char* map;
    const char* themePath;  // the zone theme class
    const char* name;
} kThemeMaps[] = {
    { "BP_ZT_TheForest", "/Game/Maps/Zones/Zone_Temperate_Forest", "/Game/World/ZoneThemes/ZT_TheForest/BP_ZT_TheForest", "Forest" },
    { "BP_ZT_TheGrasslands", "/Game/Maps/Zones/Zone_Temperate_Grassland", "/Game/World/ZoneThemes/ZT_TheGrasslands/BP_ZT_TheGrasslands", "Grassland" },
    { "BP_ZT_TheIndustrialPark", "/Game/Maps/Zones/Zone_Temperate_Industrial", "/Game/World/ZoneThemes/ZT_TheIndustrialPark/BP_ZT_TheIndustrialPark", "Industrial" },
    { "BP_ZT_TheSuburbs", "/Game/Maps/Zones/Zone_Temperate_Suburban", "/Game/World/ZoneThemes/ZT_TheSuburbs/BP_ZT_TheSuburbs", "Suburban" },
    { "BP_ZT_TheCity", "/Game/Maps/Zones/Zone_Temperate_Urban", "/Game/World/ZoneThemes/ZT_TheCity/BP_ZT_TheCity", "Urban" },
    { "BP_ZT_Onboarding_Fort", "/Game/Maps/Zones/TheFarmstead/Zone_Onboarding_FarmsteadFort", "/Game/World/ZoneThemes/Onboarding/ZT_OnboardingFort/BP_ZT_Onboarding_Fort", "Onboarding" },
    { "BP_ZT_TheOutpost_PvE_01", "/Game/Maps/Zones/Outpost/Zone_Outpost_PvE_01", "/Game/World/ZoneThemes/Outposts/BP_ZT_TheOutpost_PvE_01", "Outpost1" },
    { "BP_ZT_TheOutpost_PvE_02", "/Game/Maps/Zones/Outpost/Zone_Outpost_PvE_02", "/Game/World/ZoneThemes/Outposts/BP_ZT_TheOutpost_PvE_02", "Outpost2" },
    { "BP_ZT_TheOutpost_PvE_03", "/Game/Maps/Zones/Outpost/Zone_Outpost_PvE_03", "/Game/World/ZoneThemes/Outposts/BP_ZT_TheOutpost_PvE_03", "Outpost3" },
    { "BP_ZT_TheOutpost_PvE_04", "/Game/Maps/Zones/Outpost/Zone_Outpost_PvE_04", "/Game/World/ZoneThemes/Outposts/BP_ZT_TheOutpost_PvE_04", "Outpost4" },
};

// The launch screen. With a server, matchmaking ended in the server's pre-game lobby (UE's LobbyBeacon). The client's
// FortMatchmakingContext told the UI through its delegates: OnLobbyStarted, then OnLobbyTimeUpdated(seconds left)
// every second. The Play tab then shows Select your Hero with the lobby clock, and the zone opens when the clock runs
// out or the leader launches (JoinFromLobby). There is no server here, so the delegates are broadcast from here, the
// context's lobby calls are answered, and the zone opens when the lobby ends.
constexpr uint32_t kJoinFromLobbyExec = 0x00A40620, kReadyUpExec = 0x00A4BAA0, kCancelMatchmakingExec = 0x00A22BC0;
constexpr uint32_t kDisconnectFromLobbyExec = 0x00A27860, kIsInLobbyExec = 0x00A3F5F0;
// Launch stays greyed out ("Server starting, please wait...") until the server's world record is loaded: the launch
// control asks IsWorldRecordLoaded and listens to OnWorldRecordLoadedChanged(bool).
constexpr uint32_t kIsWorldRecordLoadedExec = 0x00A405C0;
// The outpost (the storm shield base, one per theater): FortMatchmakingContext.EnterOutpost would find the player's
// outpost server and IsOutpostPermitted greys the way in out; neither has a server to ask here. EnterOutpost opens the
// current theater's outpost zone with its own mission (Stonewood the first, Plankerton the second, Canny Valley the
// third, Twine Peaks the fourth), and the outpost is always permitted.
constexpr uint32_t kEnterOutpostExec = 0x00A28740, kIsOutpostPermittedExec = 0x00A3FBE0;
// The Play tab's locks: a tile is locked and its Start Mission greyed out while the player doesn't meet its
// requirements, the mission's power above all (up to 30 in Stonewood, 10715 in Twine Peaks, more than a homebase can
// reach). FortPlayerControllerFrontEnd::IsRequirementsMet says yes to everything when the controller's bUnlockAllZones
// is set, Epic's own switch; it's set here, so every mission can be started.
constexpr int FrontEndUnlockAllZones = 0xC64;
// The world manager has the backend validate the zone's mission (world/validate) or the zone never loads ("LoadZoneWorld
// Failed"): a mission of that theater goes along (its outpost tile's, or Stonewood's Close the Gate).
const struct {
    const char* theater;
    int outpost;
    const char* mission;
} kTheaterOutposts[] = {
    { "33A2311D4AE64B361CCE27BC9F313C8B", 1, "240487D1CE39E94760999521942A2B37" },
    { "D477605B4FA48648107B649CE97FCF27", 2, "1fc8ab75-ff67-4113-8b58-8e1511aaacb9" },
    { "E6ECBD064B153234656CB4BDE6743870", 3, "df1d9601-da18-4b09-8feb-b4ac5103e030" },
    { "D9A801C5444D1C74D1B7DAB5C7C12C5B", 4, "59e67f4a-d66c-4bc6-b3de-a80610ebc072" },
};
constexpr int OnWorldRecordLoadedChanged = 0xB8;
// UFortMatchmakingContext's multicast delegates: TArray<FScriptDelegate>, each a weak object and the function's FName.
constexpr int OnMatchmakingStarted = 0x1C, OnMatchmakingComplete = 0x28, OnLobbyStarted = 0x70, OnLobbyDisconnected = 0x7C;
constexpr int OnLobbyTimeUpdated = 0x88, OnLobbyTimeExpired = 0x94, OnLocalReadyUpStatusChange = 0xA0;
constexpr int ScriptDelegateSize = 0x10, DelegateFunctionName = 0x8;
constexpr uint32_t kWeakGet = 0x00DA4200;  // FWeakObjectPtr::Get(bool bEvenIfPendingKill), thiscall
constexpr uint8_t kMatchmakingSuccess = 5, kMatchmakingCancelled = 1;  // EMatchmakingCompleteResult
constexpr int kLobbySeconds = 60;
constexpr ULONGLONG kLobbyPollMs = 1000, kMemberWaitMs = 180000;

struct Lobby {
    bool active = false, started = false, launch = false, ready = false, cancel = false;
    uintptr_t context = 0;  // the local player's FortMatchmakingContext
    ULONGLONG endsAt = 0;
    int shown = -1;   // the seconds left last sent to the clock
    std::string url;  // the zone it opens
    // Co-op (below): the shared launch screen this one is on.
    std::string account, host;  // this player; whose launch screen it is (empty: alone)
    bool answered = false, member = false, launchSent = false;
    int players = 1;
    ULONGLONG nextPoll = 0, waitingSince = 0;
    std::string address;  // the host's zone, once announced
};
Lobby g_lobby;  // game thread; the UI's calls only set flags

// Co-op: the launch screen is shared through the backend (ot65/lobby/...). Play opens one there with this player's
// mission and clock, or puts the player on another player's open one (their mission, the same clock), or answers with
// the address of a game already running (joined straight away, as the game's matchmaking would have found a server).
// Everyone on a launch screen asks for its state every second: the clock, who launched, how many are on it, and the
// host's zone once it is announced. When it launches (the clock runs out, or anyone presses Launch) the host opens the
// zone and the others join it as soon as it listens. The answers come on worker threads; each Play is a generation and
// older answers are dropped.
struct Shared {
    int answer = 0;  // to Play: 1 opened, 2 joined another's, 3 a game runs, -1 no backend
    std::string host, address;
    int remainingMs = -1, players = 1;
    bool launched = false, closed = false, fresh = false;  // fresh: a state not yet looked at
};
std::mutex g_sharedLock;
Shared g_shared;
uint64_t g_generation = 0;
std::string g_mission, g_theater;  // the selected tile's mission and theater, for the backend

std::string g_travel;     // the zone to open on the next tick (not from inside the UI's call)
std::string g_generator;  // the mission generator for the zone being opened

std::string ReadFString(uintptr_t at) {
    const wchar_t* data = reinterpret_cast<const wchar_t*>(At<uint32_t>(at, 0));
    int32_t num = At<int32_t>(at, 4);
    std::string out;
    for (int32_t i = 0; data && i < num - 1 && i < 1024; i++) out.push_back(data[i] < 0x80 ? static_cast<char>(data[i]) : '?');
    return out;
}

// "BlueprintGeneratedClass /Game/...X.X_C" -> "/Game/...X.X_C".
std::string PathOf(uintptr_t object) {
    std::string full = engine::Describe(object);
    size_t space = full.find(' ');
    return space == std::string::npos ? full : full.substr(space + 1);
}

// A number or bool after "name": in a flat JSON answer (-1 if missing).
int JsonInt(const std::string& json, const char* name) {
    std::string key = std::string("\"") + name + "\":";
    size_t at = json.find(key);
    if (at == std::string::npos) return -1;
    at += key.size();
    if (json.compare(at, 4, "true") == 0) return 1;
    if (json.compare(at, 5, "false") == 0) return 0;
    return atoi(json.c_str() + at);
}

void ReadState(const std::string& body, Shared& state) {
    state.closed = body.find("\"closed\":true") != std::string::npos;
    state.host = backend::JsonField(body, "host");
    state.address = backend::JsonField(body, "address");
    state.remainingMs = JsonInt(body, "remainingMs");
    state.launched = JsonInt(body, "launched") == 1;
    int players = JsonInt(body, "players");
    state.players = players > 0 ? players : 1;
    state.fresh = true;
}

std::wstring LobbyPath(const std::string& host, const char* tail = "") {
    std::string path = "/fortnite/api/game/v2/ot65/lobby/" + host + tail;
    return std::wstring(path.begin(), path.end());
}

void AskState(const std::string& host) {
    uint64_t generation = g_generation;
    backend::Get(LobbyPath(host), [generation](int status, const std::string& body) {
        if (status != 200) return;
        std::lock_guard<std::mutex> guard(g_sharedLock);
        if (generation != g_generation) return;
        int answer = g_shared.answer;
        ReadState(body, g_shared);
        g_shared.answer = answer;
    });
}

void Tell(const std::string& host, const char* what, const std::string& account) {
    backend::Post(LobbyPath(host, what), "{\"accountId\":\"" + account + "\"}", nullptr);
}

// The outpost zone `n` (1-4) with its own mission, queued for the next tick.
void OpenOutpost(int n, const std::string& theater, const std::string& mission) {
    std::string zone = "Outpost" + std::to_string(n);
    for (const auto& z : kThemeMaps) {
        if (zone != z.name) continue;
        std::string name = "TheOutpost_PvE_0" + std::to_string(n);
        g_generator = "/Game/World/MissionGens/MissionGen_" + name + ".MissionGen_" + name + "_C";
        g_travel = std::string(z.map) + "?game=/Script/FortniteGame.FortGameMode?zoneid={\"worldId\":\"\",\"theaterId\":\"" + theater +
                   "\",\"theaterMissionId\":\"" + mission + "\",\"zoneThemeClass\":\"" + z.themePath + "." + z.theme + "_C\"}";
        LOG("lobby: entering outpost %d (%s)", n, z.map);
        return;
    }
}

// A map tile's mission zone, queued for the next tick. False (the game's own search goes on) if it can't be read.
bool OpenTile(uintptr_t viewer, uintptr_t tile) {
    std::string mission = ReadFString(tile + TileMissionData);
    std::string generator = ReadFString(tile + TileMissionData + MissionGeneratorRef + SoftPath);
    std::string theme = PathOf(At<uint32_t>(tile, TileZoneTheme));
    int32_t index = At<int32_t>(viewer, ViewerTheaterIndex);
    engine::TArray& theaters = At<engine::TArray>(viewer, ViewerTheaters);
    std::string theater = theaters.data && index >= 0 && index < theaters.num
                              ? ReadFString(theaters.data + static_cast<uintptr_t>(index) * TheaterSize + TheaterUniqueId) : "";
    const char* map = nullptr;
    for (const auto& entry : kThemeMaps)
        if (theme.find(std::string("/") + entry.theme + ".") != std::string::npos) map = entry.map;
    LOG("lobby: Play on %s: mission %s, %s, theater %s", engine::Name(tile).c_str(), mission.c_str(), generator.c_str(), theater.c_str());
    if (mission.empty() || generator.empty() || theater.empty()) {
        LOG("lobby: the selected tile has no mission");
        return false;
    }
    if (!map) {
        LOG("lobby: this build has no map for the zone theme %s", theme.c_str());
        return false;
    }
    if (!engine::Load(std::wstring(generator.begin(), generator.end()))) {
        LOG("lobby: this build has no mission %s", generator.c_str());
        return false;
    }
    g_generator = generator;
    g_mission = mission;
    g_theater = theater;
    g_travel = std::string(map) + "?game=/Script/FortniteGame.FortGameMode?zoneid={\"worldId\":\"\",\"theaterId\":\"" + theater +
               "\",\"theaterMissionId\":\"" + mission + "\",\"zoneThemeClass\":\"" + theme + "\"}";
    return true;
}

bool OpenSelected() {
    uintptr_t viewer = engine::FindFirstA("FortTheaterMapViewer");  // TheaterMapViewer_C in the frontend
    uintptr_t tile = viewer ? At<uint32_t>(viewer, ViewerSelectedTile) : 0;
    if (!engine::IsA(tile, "FortTheaterMapTile")) {
        LOG("lobby: no mission tile selected on the theater map");
        return false;
    }
    return OpenTile(viewer, tile);
}

// Quick Match, the hex in the middle of the map (BP_Hex_CriticalMission, no mission of its own): the server would pick
// a mission in the chosen threat range. One of the theater's mission tiles on the map is picked here, at random.
bool OpenQuickMatch(int32_t minPower, int32_t maxPower) {
    uintptr_t viewer = engine::FindFirstA("FortTheaterMapViewer");
    if (!viewer) return false;
    std::vector<uintptr_t> tiles;
    for (uintptr_t tile : engine::FindAllA("FortTheaterMapTile")) {
        std::string theme = PathOf(At<uint32_t>(tile, TileZoneTheme));
        bool mission = false;
        for (const auto& entry : kThemeMaps)
            if (theme.find(std::string("/") + entry.theme + ".") != std::string::npos)
                mission = strncmp(entry.name, "Outpost", 7) != 0 && strcmp(entry.name, "Onboarding") != 0;
        if (mission && !ReadFString(tile + TileMissionData).empty()) tiles.push_back(tile);
    }
    LOG("lobby: Quick Match (power %d-%d): %zu mission tile(s) on the map", minPower, maxPower, tiles.size());
    return !tiles.empty() && OpenTile(viewer, tiles[GetTickCount() % tiles.size()]);
}

void Finish(void* stack) {
    uint32_t& code = At<uint32_t>(reinterpret_cast<uintptr_t>(stack), FrameCode);
    if (code) code++;  // P_FINISH, as the original exec does before its call
}

void CallExec(uint32_t exec, uintptr_t context, void* stack, void* result) {
    reinterpret_cast<void(__thiscall*)(uintptr_t, void*, void*)>(engine::Va(exec))(context, stack, result);
}

void StartLaunchScreen(uintptr_t context);

void __fastcall ExecStartMatchmaking(uintptr_t context, void*, void* stack, void* result) {
    if (g_lobby.active) {  // a launch from the launch screen itself
        Finish(stack);
        g_lobby.launch = true;
        return;
    }
    if (OpenSelected()) {
        Finish(stack);
        StartLaunchScreen(context);
        return;
    }
    CallExec(kStartMatchmakingExec, context, stack, result);
}

void __fastcall StartCriticalMission(uintptr_t context, void*, int32_t minPower, int32_t maxPower) {
    if (g_lobby.active) {
        g_lobby.launch = true;
        return;
    }
    if (OpenSelected() || OpenQuickMatch(minPower, maxPower)) {
        StartLaunchScreen(context);
        return;
    }
    reinterpret_cast<void(__thiscall*)(uintptr_t, int32_t, int32_t)>(engine::Va(kStartCriticalMission))(context, minPower, maxPower);
}

// The launch screen for the zone in g_travel; Advance opens the zone (or joins another player's) when it ends.
void StartLaunchScreen(uintptr_t context) {
    g_lobby = Lobby{};
    g_lobby.active = true;
    g_lobby.context = context;
    g_lobby.url.swap(g_travel);
    g_lobby.account = backend::AccountId(engine::LocalPlayerController());
    uint64_t generation;
    {
        std::lock_guard<std::mutex> guard(g_sharedLock);
        generation = ++g_generation;
        g_shared = Shared{};
    }
    if (g_lobby.account.empty()) return;  // no account id: alone, as before
    std::string json = "{\"accountId\":\"" + g_lobby.account + "\",\"missionId\":\"" + g_mission + "\",\"theaterId\":\"" +
                       g_theater + "\",\"seconds\":" + std::to_string(kLobbySeconds) + "}";
    backend::Post(L"/fortnite/api/game/v2/ot65/lobby/play", json, [generation](int status, const std::string& body) {
        Shared answer;
        if (status == 200) ReadState(body, answer);
        answer.answer = status != 200                                   ? -1
                        : body.find("\"joined\"") != std::string::npos  ? 2
                        : body.find("\"running\"") != std::string::npos ? 3
                                                                         : 1;
        std::lock_guard<std::mutex> guard(g_sharedLock);
        if (generation == g_generation) g_shared = answer;
    });
}

// The launch screen's buttons and questions, answered here while it is up (the game's own otherwise).
void __fastcall ExecJoinFromLobby(uintptr_t context, void*, void* stack, void* result) {
    if (!g_lobby.active) return CallExec(kJoinFromLobbyExec, context, stack, result);
    Finish(stack);
    g_lobby.launch = true;
}

void __fastcall ExecReadyUp(uintptr_t context, void*, void* stack, void* result) {
    if (!g_lobby.active) return CallExec(kReadyUpExec, context, stack, result);
    Finish(stack);
    g_lobby.ready = true;
}

void __fastcall ExecCancelMatchmaking(uintptr_t context, void*, void* stack, void* result) {
    if (!g_lobby.active) return CallExec(kCancelMatchmakingExec, context, stack, result);
    Finish(stack);
    g_lobby.cancel = true;
}

void __fastcall ExecDisconnectFromLobby(uintptr_t context, void*, void* stack, void* result) {
    if (!g_lobby.active) return CallExec(kDisconnectFromLobbyExec, context, stack, result);
    Finish(stack);
    g_lobby.cancel = true;
}

// The outpost menu's Leave with Party (Outpost_MainWidget.LeavePartyAndGame) only calls FortPartyContext.LeaveParty:
// the real party service took a player who left their party out of the zone. A party of one leaves nothing here, so in
// a zone it goes back to the lobby instead (next tick, as the results screen's leave does); in the lobby it leaves the
// party as before.
constexpr uint32_t kLeavePartyExec = 0x00A41340;
bool g_leaveZone = false;

void __fastcall ExecLeaveParty(uintptr_t context, void*, void* stack, void* result) {
    uintptr_t gameMode = engine::GameMode();
    if (!gameMode || engine::Name(At<uint32_t>(gameMode, 0xC)) != "FortGameMode") return CallExec(kLeavePartyExec, context, stack, result);
    Finish(stack);
    g_leaveZone = true;
}

void __fastcall ExecIsInLobby(uintptr_t context, void*, void* stack, void* result) {
    if (!g_lobby.active) return CallExec(kIsInLobbyExec, context, stack, result);
    Finish(stack);
    *static_cast<bool*>(result) = true;
}

std::string CurrentTheater() {
    uintptr_t viewer = engine::FindFirstA("FortTheaterMapViewer");
    if (!viewer) return "";
    int32_t index = At<int32_t>(viewer, ViewerTheaterIndex);
    engine::TArray& theaters = At<engine::TArray>(viewer, ViewerTheaters);
    return theaters.data && index >= 0 && index < theaters.num
               ? ReadFString(theaters.data + static_cast<uintptr_t>(index) * TheaterSize + TheaterUniqueId) : "";
}

void __fastcall ExecEnterOutpost(uintptr_t context, void*, void* stack, void* result) {
    Finish(stack);
    std::string theater = CurrentTheater();
    const auto* chosen = &kTheaterOutposts[0];  // Stonewood's when the theater map can't say
    for (const auto& t : kTheaterOutposts)
        if (theater == t.theater) chosen = &t;
    OpenOutpost(chosen->outpost, chosen->theater, chosen->mission);
    *static_cast<bool*>(result) = true;
}

void __fastcall ExecIsOutpostPermitted(uintptr_t context, void*, void* stack, void* result) {
    Finish(stack);
    *static_cast<bool*>(result) = true;
}

void __fastcall ExecIsWorldRecordLoaded(uintptr_t context, void*, void* stack, void* result) {
    if (!g_lobby.active) return CallExec(kIsWorldRecordLoadedExec, context, stack, result);
    Finish(stack);
    *static_cast<bool*>(result) = true;
}

// FMulticastScriptDelegate::ProcessMulticastDelegate: each bound object's function of that name, given the parameters.
void Broadcast(uintptr_t object, int delegate, void* params) {
    engine::TArray& list = At<engine::TArray>(object, delegate);
    std::vector<uint8_t> copy(list.data && list.num > 0 ? static_cast<size_t>(list.num) * ScriptDelegateSize : 0);
    if (!copy.empty()) memcpy(copy.data(), reinterpret_cast<const void*>(list.data), copy.size());
    auto weakGet = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, bool)>(engine::Va(kWeakGet));
    for (size_t at = 0; at < copy.size(); at += ScriptDelegateSize) {
        uintptr_t entry = reinterpret_cast<uintptr_t>(copy.data()) + at;
        uintptr_t target = weakGet(entry, false);
        std::string name = engine::NameOf(At<int32_t>(entry, DelegateFunctionName));
        uintptr_t function = target ? engine::FindFunctionOn(At<uint32_t>(target, ObjectClass), name.c_str()) : 0;
        if (function) engine::ProcessEvent(target, function, params);
    }
}

// Who listens to a delegate ("Lobby-LaunchControl_C.LobbyStartedEvent, ..."), for the log.
std::string Bound(uintptr_t object, int delegate) {
    engine::TArray& list = At<engine::TArray>(object, delegate);
    auto weakGet = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, bool)>(engine::Va(kWeakGet));
    std::string names;
    for (int32_t i = 0; list.data && i < list.num && i < 16; i++) {
        uintptr_t entry = list.data + static_cast<uintptr_t>(i) * ScriptDelegateSize;
        uintptr_t target = weakGet(entry, false);
        names += std::string(names.empty() ? "" : ", ") + (target ? engine::Name(At<uint32_t>(target, ObjectClass)) : "(gone)") + "." +
                 engine::NameOf(At<int32_t>(entry, DelegateFunctionName));
    }
    return names.empty() ? "nobody" : names;
}

void LeaveLaunchScreen(uintptr_t context, const char* why) {
    alignas(4) uint8_t params[16] = {};
    params[0] = kMatchmakingCancelled;
    Broadcast(context, OnMatchmakingComplete, params);
    params[0] = 0;
    Broadcast(context, OnLobbyDisconnected, params);
    LOG("lobby: %s", why);
    if (!g_lobby.host.empty()) Tell(g_lobby.host, "/leave", g_lobby.account);
    {
        std::lock_guard<std::mutex> guard(g_sharedLock);
        g_generation++;
    }
    g_lobby = Lobby{};
    g_generator.clear();
}

void AdvanceLobby(uint64_t now) {
    uintptr_t context = g_lobby.context;
    alignas(4) uint8_t params[64] = {};
    Shared shared;
    {
        std::lock_guard<std::mutex> guard(g_sharedLock);
        shared = g_shared;
        g_shared.fresh = false;
    }
    if (!engine::IsLive(context)) {
        LOG("lobby: the matchmaking context went away; no launch screen");
        if (!g_lobby.host.empty()) Tell(g_lobby.host, "/leave", g_lobby.account);
        g_lobby = Lobby{};
        return;
    }
    if (g_lobby.cancel) return LeaveLaunchScreen(context, "left the launch screen");

    // The backend's answer to Play.
    if (shared.answer && !g_lobby.answered) {
        g_lobby.answered = true;
        if (shared.answer == 3 && !shared.address.empty()) {  // another player's game runs: join it instead
            Broadcast(context, OnLobbyTimeExpired, params);
            LOG("lobby: another player's game runs at %s; joining it", shared.address.c_str());
            {
                std::lock_guard<std::mutex> guard(g_sharedLock);
                g_generation++;
            }
            g_lobby = Lobby{};
            g_generator.clear();
            net::Join(shared.address);
            return;
        }
        if (shared.answer == 1 || shared.answer == 2) {
            g_lobby.host = shared.host;
            g_lobby.member = shared.answer == 2;
            g_lobby.players = shared.players;
            LOG("lobby: %s", g_lobby.member ? ("on " + shared.host + "'s launch screen (" + std::to_string(shared.players) +
                                               " players): their mission and clock; their zone is joined when it launches").c_str()
                                            : "this launch screen is open to other players: whoever presses Play now joins this mission");
        } else {
            LOG("lobby: the backend didn't answer; this launch screen is this player's alone");
        }
    }

    if (!g_lobby.started) {
        g_lobby.started = true;
        g_lobby.endsAt = now + kLobbySeconds * 1000ULL;
        Broadcast(context, OnMatchmakingStarted, params);
        params[0] = kMatchmakingSuccess;
        Broadcast(context, OnMatchmakingComplete, params);
        params[0] = 0;
        Broadcast(context, OnLobbyStarted, params);
        params[0] = 1;  // the "server" is ready at once: Launch can be pressed
        Broadcast(context, OnWorldRecordLoadedChanged, params);
        params[0] = 0;
        LOG("lobby: the launch screen is up; the zone opens in %d s, or at Launch. OnLobbyStarted: %s; OnLobbyTimeUpdated: %s",
            kLobbySeconds, Bound(context, OnLobbyStarted).c_str(), Bound(context, OnLobbyTimeUpdated).c_str());
    }

    // The shared launch screen's state, every second.
    if (!g_lobby.host.empty() && now >= g_lobby.nextPoll) {
        g_lobby.nextPoll = now + kLobbyPollMs;
        AskState(g_lobby.host);
    }
    if (shared.fresh && g_lobby.answered && !g_lobby.host.empty()) {
        if (shared.closed) {
            if (g_lobby.member && g_lobby.address.empty()) {
                LOG("lobby: %s's launch screen closed; this one goes on alone with its own mission", g_lobby.host.c_str());
                g_lobby.member = false;
                g_lobby.launch = false;
                g_lobby.waitingSince = 0;
                g_lobby.endsAt = now + kLobbySeconds * 1000ULL;
            }
            g_lobby.host.clear();
        } else {
            if (shared.remainingMs >= 0 && !g_lobby.launch) g_lobby.endsAt = now + static_cast<ULONGLONG>(shared.remainingMs);
            if (shared.players != g_lobby.players) {
                LOG("lobby: %d player(s) on the launch screen", shared.players);
                g_lobby.players = shared.players;
            }
            if (shared.launched && !g_lobby.launch) {
                LOG("lobby: launched by another player");
                g_lobby.launch = true;
                g_lobby.launchSent = true;
            }
            if (g_lobby.member && !shared.address.empty()) g_lobby.address = shared.address;
        }
    }

    if (g_lobby.ready) {  // alone, the player being ready is everyone being ready
        g_lobby.ready = false;
        params[0] = 1;
        Broadcast(context, OnLocalReadyUpStatusChange, params);
        params[0] = 0;
        g_lobby.launch = true;
    }
    int left = g_lobby.launch || now >= g_lobby.endsAt ? 0 : static_cast<int>((g_lobby.endsAt - now + 999) / 1000);
    if (left != g_lobby.shown) {
        g_lobby.shown = left;
        int32_t seconds = left;
        memcpy(params, &seconds, sizeof(seconds));
        Broadcast(context, OnLobbyTimeUpdated, params);
        memset(params, 0, sizeof(seconds));
    }
    if (left > 0 && !g_lobby.launch) return;
    if (!g_lobby.launchSent && !g_lobby.host.empty()) {  // everyone on it launches with this one
        g_lobby.launchSent = true;
        Tell(g_lobby.host, "/launch", g_lobby.account);
    }

    if (g_lobby.member) {  // the host opens the zone; join it once it listens
        if (!g_lobby.waitingSince) {
            g_lobby.waitingSince = now;
            LOG("lobby: launched; waiting for %s's zone", g_lobby.host.c_str());
        }
        if (!g_lobby.address.empty()) {
            Broadcast(context, OnLobbyTimeExpired, params);
            std::string address = g_lobby.address;
            LOG("lobby: %s's zone is up at %s; joining it", g_lobby.host.c_str(), address.c_str());
            {
                std::lock_guard<std::mutex> guard(g_sharedLock);
                g_generation++;
            }
            g_lobby = Lobby{};
            g_generator.clear();
            net::Join(address);
            return;
        }
        if (now - g_lobby.waitingSince > kMemberWaitMs) LeaveLaunchScreen(context, "the host's zone never opened; left the launch screen");
        return;
    }

    if (!left) Broadcast(context, OnLobbyTimeExpired, params);
    LOG("lobby: %s; opening the zone%s", g_lobby.launch ? "launched" : "the lobby clock ran out",
        g_lobby.players > 1 ? (" (" + std::to_string(g_lobby.players - 1) + " other player(s) join it once it is up)").c_str() : "");
    {
        std::lock_guard<std::mutex> guard(g_sharedLock);
        g_generation++;  // late answers are ignored
    }
    g_travel.swap(g_lobby.url);
    g_lobby = Lobby{};
}

void __fastcall ExecIsEntirePartyInFrontEnd(uintptr_t context, void*, void* stack, void* result) {
    reinterpret_cast<void(__thiscall*)(uintptr_t, void*, void*)>(engine::Va(kIsEntirePartyInFrontEndExec))(context, stack, result);
    bool& inFrontEnd = *static_cast<bool*>(result);
    if (inFrontEnd || !engine::IsA(engine::GameMode(), "FortGameModeFrontEnd")) return;
    // Only alone: with real party members the game's own answer stands.
    struct {
        int32_t count;
    } p{ 0 };
    engine::ProcessEvent(context, engine::FindFunction("/Script/FortniteGame.FortPartyContext.GetNumPlayersInLocalParty"), &p);
    if (p.count <= 1) inFrontEnd = true;
}

std::wstring ReadWide(uintptr_t at) {
    const wchar_t* data = reinterpret_cast<const wchar_t*>(At<uint32_t>(at, 0));
    int32_t num = At<int32_t>(at, 4);
    return data && num > 1 ? std::wstring(data, num - 1) : std::wstring();
}
void UnlockAllZones() {
    static uintptr_t seen = 0;
    uintptr_t controller = engine::LocalPlayerController();
    if (!controller || controller == seen) return;
    seen = controller;
    if (!engine::IsA(controller, "FortPlayerControllerFrontEnd")) return;
    At<uint8_t>(controller, FrontEndUnlockAllZones) = 1;
    LOG("lobby: every zone unlocked (bUnlockAllZones)");
}
}  // namespace

bool Install() {
    uintptr_t function = engine::FindFunction("/Script/FortniteGame.FortMatchmakingContext.StartMatchmaking");
    if (!function || At<uint32_t>(function, FunctionNative) != engine::Va(kStartMatchmakingExec)) {
        LOG("lobby: FortMatchmakingContext.StartMatchmaking not found or not the expected native: the Play tab stays as it was");
        return false;
    }
    At<uint32_t>(function, FunctionNative) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ExecStartMatchmaking));
    if (!engine::PatchCall(kStartCriticalMissionCall, kStartCriticalMission, reinterpret_cast<void*>(&StartCriticalMission)))
        LOG("lobby: StartCriticalMission's call not found: a quest's mission tile still searches for a server");
    const struct {
        const char* function;
        uint32_t exec;
        void* replacement;
    } kLaunchScreenCalls[] = {
        { "/Script/FortniteGame.FortMatchmakingContext.JoinFromLobby", kJoinFromLobbyExec, &ExecJoinFromLobby },
        { "/Script/FortniteGame.FortMatchmakingContext.ReadyUp", kReadyUpExec, &ExecReadyUp },
        { "/Script/FortniteGame.FortMatchmakingContext.CancelMatchmaking", kCancelMatchmakingExec, &ExecCancelMatchmaking },
        { "/Script/FortniteGame.FortMatchmakingContext.DisconnectFromLobby", kDisconnectFromLobbyExec, &ExecDisconnectFromLobby },
        { "/Script/FortniteGame.FortMatchmakingContext.IsInLobby", kIsInLobbyExec, &ExecIsInLobby },
        { "/Script/FortniteGame.FortPartyContext.LeaveParty", kLeavePartyExec, &ExecLeaveParty },
        { "/Script/FortniteGame.FortMatchmakingContext.IsWorldRecordLoaded", kIsWorldRecordLoadedExec, &ExecIsWorldRecordLoaded },
        { "/Script/FortniteGame.FortMatchmakingContext.EnterOutpost", kEnterOutpostExec, &ExecEnterOutpost },
        { "/Script/FortniteGame.FortMatchmakingContext.IsOutpostPermitted", kIsOutpostPermittedExec, &ExecIsOutpostPermitted },
    };
    for (const auto& call : kLaunchScreenCalls) {
        uintptr_t wrapped = engine::FindFunction(call.function);
        if (wrapped && At<uint32_t>(wrapped, FunctionNative) == engine::Va(call.exec))
            At<uint32_t>(wrapped, FunctionNative) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(call.replacement));
        else
            LOG("lobby: %s not found or not the expected native", call.function);
    }
    uintptr_t party = engine::FindFunction("/Script/FortniteGame.FortPartyContext.IsEntirePartyInFrontEnd");
    bool partyWrapped = party && At<uint32_t>(party, FunctionNative) == engine::Va(kIsEntirePartyInFrontEndExec);
    if (partyWrapped)
        At<uint32_t>(party, FunctionNative) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ExecIsEntirePartyInFrontEnd));
    LOG("lobby: the Play tab opens the selected mission here%s", partyWrapped ? "; a party of one is always in Homebase" : "");
    return true;
}

void Advance(uint64_t now) {
    UnlockAllZones();
    if (g_leaveZone) {
        g_leaveZone = false;
        LOG("lobby: left the party in a zone; back to the lobby");
        endzone::RequestLeave();
    }
    if (g_lobby.active) AdvanceLobby(now);
    if (g_travel.empty()) return;
    std::string url;
    url.swap(g_travel);
    LOG("lobby: opening %s", url.c_str());
    trace::Execute("travel " + url);
}

std::wstring MissionInfo() {
    if (g_generator.empty()) return L"";
    uintptr_t generator = engine::Load(std::wstring(g_generator.begin(), g_generator.end()));
    uintptr_t defaults = generator ? At<uint32_t>(generator, ClassDefaultObject) : 0;
    if (!defaults) {
        LOG("lobby: no mission generator %s", g_generator.c_str());
        return L"";
    }
    std::wstring path = ReadWide(defaults + GeneratorMissionInfo + SoftPath);
    if (path.empty()) path = ReadWide(defaults + GeneratorPrimaryMissionInfo + SoftPath);
    LOG("lobby: %s's mission is %ls", g_generator.c_str(), path.empty() ? L"(none)" : path.c_str());
    return path;
}

void ForgetMission() {
    g_generator.clear();
}

// `outpost [1-4]`: that outpost (no number: the current theater's), as Enter Outpost opens it.
void Outpost(int n) {
    std::string theater = CurrentTheater();
    const auto* chosen = &kTheaterOutposts[0];
    for (const auto& t : kTheaterOutposts)
        if (n ? t.outpost == n : theater == t.theater) chosen = &t;
    OpenOutpost(chosen->outpost, chosen->theater, chosen->mission);
}

// `playmission <generator> [Forest|Grassland|Industrial|Suburban|Urban|Onboarding|Outpost1..4]`: a zone with
// /Game/World/MissionGens/MissionGen_<generator> straight away (no launch screen), as the Play tab opens one.
void PlayMission(const std::string& arg) {
    size_t space = arg.find(' ');
    std::string name = arg.substr(0, space), zone = space == std::string::npos ? "Forest" : arg.substr(space + 1);
    const char* map = nullptr;
    std::string themePath;
    for (const auto& z : kThemeMaps) {
        if (zone != z.name) continue;
        map = z.map;
        themePath = std::string(z.themePath) + "." + z.theme + "_C";
    }
    std::string generator = "/Game/World/MissionGens/MissionGen_" + name + ".MissionGen_" + name + "_C";
    if (!map || !engine::Load(std::wstring(generator.begin(), generator.end())) || !engine::Load(std::wstring(themePath.begin(), themePath.end()))) {
        LOG("lobby: playmission <generator> [Forest|Grassland|Industrial|Suburban|Urban|Onboarding|Outpost1..4]: no %s or %s",
            generator.c_str(), themePath.empty() ? zone.c_str() : themePath.c_str());
        return;
    }
    g_generator = generator;
    g_travel = std::string(map) + "?game=/Script/FortniteGame.FortGameMode?zoneid={\"worldId\":\"\","
               "\"theaterId\":\"33A2311D4AE64B361CCE27BC9F313C8B\",\"theaterMissionId\":\"240487D1CE39E94760999521942A2B37\","
               "\"zoneThemeClass\":\"" + themePath + "\"}";
    LOG("lobby: playmission %s in %s", generator.c_str(), zone.c_str());
}

// `lobbyplay [generator name part]`: Play on the Play tab, as a click does, with the theater map's first mission tile
// whose generator has the part (any mission tile without one) selected first. For tests of the launch screen.
void PlayFromTab(const std::string& part) {
    uintptr_t viewer = engine::FindFirstA("FortTheaterMapViewer");
    uintptr_t context = engine::FindFirstA("FortMatchmakingContext");
    if (!viewer || !context) {
        LOG("lobby: no theater map (%s) or matchmaking context (%s) here", engine::Describe(viewer).c_str(), engine::Describe(context).c_str());
        return;
    }
    uintptr_t chosen = 0;
    std::string generators;
    for (uintptr_t tile : engine::FindAllA("FortTheaterMapTile")) {
        std::string generator = ReadFString(tile + TileMissionData + MissionGeneratorRef + SoftPath);
        if (generator.empty()) continue;
        std::string name = generator.substr(generator.rfind('.') + 1);
        if (generators.find(" " + name) == std::string::npos) generators += " " + name;
        if (!chosen && (part.empty() || generator.find(part) != std::string::npos)) chosen = tile;
    }
    if (!chosen) {
        // The theater map fills in once the Play tab is open: without it, the same launch screen for Close the Gate in
        // Stonewood (the `play` zone), with nobody's UI on it.
        LOG("lobby: no mission tile like \"%s\" (generators on the map:%s); the launch screen for Close the Gate", part.c_str(),
            generators.empty() ? " none, the Play tab isn't open" : generators.c_str());
        g_generator.clear();
        g_mission = "240487D1CE39E94760999521942A2B37";
        g_theater = "33A2311D4AE64B361CCE27BC9F313C8B";
        g_travel = "/Game/Maps/Zones/Zone_Temperate_Forest?game=/Script/FortniteGame.FortGameMode?zoneid={\"worldId\":\"\","
                   "\"theaterId\":\"" + g_theater + "\",\"theaterMissionId\":\"" + g_mission + "\","
                   "\"zoneThemeClass\":\"/Game/World/ZoneThemes/ZT_TheForest/BP_ZT_TheForest.BP_ZT_TheForest_C\"}";
        StartLaunchScreen(context);
        return;
    }
    At<uint32_t>(viewer, ViewerSelectedTile) = static_cast<uint32_t>(chosen);
    LOG("lobby: Play with %s selected (generators on the map:%s)", engine::Name(chosen).c_str(), generators.c_str());
    alignas(8) uint8_t params[64] = {};
    engine::ProcessEvent(context, engine::FindFunction("/Script/FortniteGame.FortMatchmakingContext.StartMatchmaking"), params);
}

}  // namespace lobby
