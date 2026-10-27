#include "backend.h"
#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <cstring>

#pragma comment(lib, "user32.lib")  // GetAsyncKeyState, GetForegroundWindow

namespace endzone {

using engine::At;

namespace {
// The end of a zone is server code that is compiled out: the mission manager's zone-complete slot (vslot 194) is a
// 'ret 4', nothing sets the game state's GameplayState to EndOfZone/LeavingZone, and the results screen's leave request
// ends in FortGameMode vslot 251, a 'ret 8'. The client half survives: GameplayState 2 makes the HUD show the results
// layer ("Victory!"/"Defeat!", "Returning to town map in N s" from VoteTimeLeft), and ClientReturnToMainMenu takes the
// player back to the frontend. The steps here run from world ticks only, never from inside a UI callback.
constexpr uint32_t kZoneCompletedEntry = 0x027428DC, kZoneCompletedStub = 0x019E8750;    // MissionManager (1), ret 4
constexpr uint32_t kGameplayActionEntry = 0x02704EEC, kGameplayActionStub = 0x00947E60;  // FortGameMode (pc, action), ret 8
constexpr uint32_t kTextCtor = 0x00C8BA20;     // FText::FText(), thiscall
constexpr uint32_t kZoneEnded = 0x0201A9C0;    // UFortHUDContext: broadcasts OnZoneCompleted (fallback), thiscall
constexpr int MissionManagerMissions = 0x268, MissionInfo = 0x24C, MissionInfoSecondary = 0x34;
constexpr int MissionStatus = 0x280, MissionGuid = 0x2B4;
constexpr int GameStateGameplayState = 0x330, GameStateVoteTimeLeft = 0x2B4, GameStateLeaveTimeLeft = 0x3D4;
constexpr int GameStateMissionManager = 0x2CC;
constexpr int VtSetGameplayState = 0x320;      // AFortGameState (uint8), ret 4
constexpr int PlayerControllerPlayer = 0x288;  // UFortLocalPlayer
constexpr int LocalPlayerRewardActivities = 0x10C + 0x24;  // CachedRewardReport.RewardActivities
constexpr int ActivitySize = 0x4C, ActivityGuid = 0x4, ActivityTitle = 0x14, ActivityDescription = 0x20, ActivityResult = 0x48;
constexpr ULONGLONG kResultsDelayMs = 3000, kHudFallbackMs = 1000, kWatchdogMs = 6000, kFrontendUiMs = 10000;
constexpr ULONGLONG kClickGraceMs = 2000;  // a click this soon after the results appear is still the fight's
constexpr int kCountdown = 30;

enum Stage { kIdle, kResults, kLeaving };
struct State {
    Stage stage = kIdle;
    bool won = false, leaveRequested = false, hudShown = false, hudFallbackDone = false, watchdogDone = false;
    bool mouseDown = true;  // a press counts only after a release was seen on the results screen
    int32_t secondsLeft = 0;
    uint8_t guid[16] = {};
    ULONGLONG endAt = 0, resultsAt = 0, nextSecond = 0, leaveAt = 0;
    uintptr_t world = 0;
};
State g_state;
uintptr_t g_clientReportWorld = 0;  // a co-op player who joined: the zone whose results they already have
volatile bool g_active = false;   // anything to do on world ticks (keeps the idle path free)
ULONGLONG g_returnedAt = 0;       // left a zone: check the frontend UI once it is loaded
uintptr_t g_returnedFrom = 0;

bool IsZone() {
    uintptr_t gameMode = engine::GameMode();
    return gameMode && engine::Name(At<uint32_t>(gameMode, 0xC)) == "FortGameMode";
}

void SetGameplayState(uintptr_t gameState, uint8_t state) {
    reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t)>(At<uint32_t>(At<uint32_t>(gameState, 0), VtSetGameplayState))(gameState, state);
}

void Call(uintptr_t object, const char* path, void* params) {
    uintptr_t function = engine::FindFunction(path);
    if (object && function) engine::ProcessEvent(object, function, params);
    else LOG("endzone: can't call %s", path);
}

// The results layer takes its title from the local player's cached reward report: a MissionPrimary activity whose
// result byte is 0 for "Victory!" and 1 for "Defeat!".
void WriteRewardReport(uintptr_t pc, bool won, const uint8_t* guid) {
    uintptr_t localPlayer = At<uint32_t>(pc, PlayerControllerPlayer);
    if (!engine::IsA(localPlayer, "FortLocalPlayer")) return;
    engine::TArray& activities = At<engine::TArray>(localPlayer, LocalPlayerRewardActivities);
    for (int32_t i = 0; activities.data && i < activities.num; i++) {
        uintptr_t activity = activities.data + static_cast<uintptr_t>(i) * ActivitySize;
        if (At<uint8_t>(activity, 0) == 1) {
            At<uint8_t>(activity, ActivityResult) = won ? 0 : 1;
            return;
        }
    }
    alignas(4) uint8_t activity[ActivitySize] = {};
    auto textCtor = reinterpret_cast<void*(__thiscall*)(void*)>(engine::Va(kTextCtor));
    textCtor(activity + ActivityTitle);  // the report's owner destroys these texts later, so real empty FTexts
    textCtor(activity + ActivityDescription);
    activity[0] = 1;  // EFortRewardActivityType::MissionPrimary
    memcpy(activity + ActivityGuid, guid, 16);
    activity[ActivityResult] = won ? 0 : 1;
    engine::Append(activities, activity, ActivitySize);
}

// The backend puts the mission's rewards into the player's profile (src/routes/ot65.js in Fortify-Alpha); the client
// picks them up with its next profile call in the lobby.
void ReportMission(uintptr_t pc) {
    std::string account = backend::AccountId(pc), zone = backend::ZoneInstanceId();
    std::string theater = backend::JsonField(zone, "theaterId"), mission = backend::JsonField(zone, "theaterMissionId");
    if (account.empty() || theater.empty() || mission.empty()) {
        LOG("endzone: no rewards: account \"%s\", theater \"%s\", mission \"%s\"", account.c_str(), theater.c_str(),
            mission.c_str());
        return;
    }
    std::string json = "{\"accountId\":\"" + account + "\",\"theaterId\":\"" + theater + "\",\"missionGuid\":\"" + mission +
                       "\",\"victory\":" + (g_state.won ? "true" : "false") + "}";
    backend::Post(L"/fortnite/api/game/v2/ot65/mission/end", json, [](int status, const std::string& body) {
        LOG("endzone: rewards: the backend answered %d %s", status, body.substr(0, 300).c_str());
    });
}

// The results layer has no button: the left mouse button, pressed in the game window once the results are up, leaves
// at once instead of waiting for the countdown.
bool ClickedOnResults(ULONGLONG now) {
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool pressed = down && !g_state.mouseDown;
    g_state.mouseDown = down;
    if (!pressed || now - g_state.resultsAt < kClickGraceMs) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

// A co-op player who joined goes back to the lobby: their backpack is saved first (the host keeps their inventory),
// then their own copy of the game is told to return, as the host's player is (ClientReturnToMainMenu reaches it).
void SendHome(uintptr_t pc) {
    loadout::Save(pc);
    uint8_t stay[4] = { 1 };
    Call(pc, "/Script/FortniteGame.FortPlayerController.ClientStayWithParty", stay);
    struct {
        const wchar_t* data;
        int32_t num, max;
    } reason{};
    Call(pc, "/Script/Engine.PlayerController.ClientReturnToMainMenu", &reason);
    LOG("endzone: %s goes back to the lobby", engine::Name(pc).c_str());
}

void ShowResults(ULONGLONG now) {
    uintptr_t pc = engine::LocalPlayerController(), gameState = engine::GameState();
    if (pc) ReportMission(pc);
    for (uintptr_t joined : net::JoinedControllers()) ReportMission(joined);  // every player's rewards, by account
    if (!pc || !engine::IsA(gameState, "FortGameStateZone")) {
        LOG("endzone: no Zone game state; skipping the results screen");
        g_state.stage = kResults;
        g_state.secondsLeft = kCountdown;
        g_state.nextSecond = now + 1000;
        return;
    }
    WriteRewardReport(pc, g_state.won, g_state.guid);
    if (game::g_on.results) SetGameplayState(gameState, 2);  // EndOfZone: the HUD switches to the results layer
    // After the switch, which resets the vote timer; the countdown is kept here and only shown through these fields.
    g_state.secondsLeft = kCountdown;
    At<int32_t>(gameState, GameStateVoteTimeLeft) = kCountdown;
    At<int32_t>(gameState, GameStateLeaveTimeLeft) = kCountdown;
    g_state.stage = kResults;
    g_state.resultsAt = now;
    g_state.nextSecond = now + 1000;
    LOG("endzone: %s; results %s, back to the lobby in %d s (or press leave)", g_state.won ? "victory" : "defeat",
        game::g_on.results ? "shown" : "off", kCountdown);
}

// The HUD learns about the end through a binding made when its context saw the game state; if that binding was
// missed, the context's own handler is called directly.
void HudFallback() {
    uintptr_t pc = engine::LocalPlayerController();
    uintptr_t library = engine::FindObject("/Script/FortniteGame.Default__BlueprintContextLibrary", "BlueprintContextLibrary");
    uintptr_t contextClass = engine::FindClass("/Script/FortniteUI.FortHUDContext");
    struct {
        uintptr_t object, cls, result;
    } p{ pc, contextClass, 0 };
    Call(library, "/Script/FortniteGame.BlueprintContextLibrary.GetContext", &p);
    if (engine::IsA(p.result, "FortHUDContext")) {
        reinterpret_cast<void(__thiscall*)(uintptr_t)>(engine::Va(kZoneEnded))(p.result);
        LOG("endzone: the HUD had not reacted; called its zone-completed handler directly");
    }
}

void Leave(ULONGLONG now) {
    uintptr_t pc = engine::LocalPlayerController(), gameState = engine::GameState();
    for (uintptr_t joined : net::JoinedControllers()) SendHome(joined);  // before the host's world (their server) goes
    if (pc && IsZone()) loadout::Save(pc);  // before the world and its inventory go
    if (engine::IsA(gameState, "FortGameStateZone")) {
        SetGameplayState(gameState, 4);  // LeavingZone: no more respawns
        At<int32_t>(gameState, GameStateVoteTimeLeft) = -1;
        At<int32_t>(gameState, GameStateLeaveTimeLeft) = 0;
    }
    director::Reset();  // the patched getters return null from here on, as the stub did; the director dies with the world
    uint8_t stay[4] = { 1 };
    Call(pc, "/Script/FortniteGame.FortPlayerController.ClientStayWithParty", stay);
    struct {
        const wchar_t* data;
        int32_t num, max;
    } reason{};
    Call(pc, "/Script/Engine.PlayerController.ClientReturnToMainMenu", &reason);
    g_state.stage = kLeaving;
    g_state.leaveAt = now;
    g_returnedAt = now;
    g_returnedFrom = g_state.world;
    LOG("endzone: returning to the lobby");
}

// If the return did not start (a session or party path refused), travel to the frontend map directly.
void Watchdog() {
    uintptr_t pc = engine::LocalPlayerController();
    struct {
        const wchar_t* data;
        int32_t num, max;
        uint8_t travelType, seamless, pad[2];
        uint8_t guid[16];
    } p{ L"/Game/Maps/FortniteEntry", 25, 25, 0 /*TRAVEL_Absolute*/, 0, {}, {} };
    Call(pc, "/Script/Engine.PlayerController.ClientTravel", &p);
    LOG("endzone: still in the zone; travelling to /Game/Maps/FortniteEntry directly");
}

// Back in the frontend: its level blueprint normally switches the UI to FrontEnd (3); if it did not, do it.
void CheckFrontend(ULONGLONG now) {
    if (now - g_returnedAt < kFrontendUiMs || engine::World() == g_returnedFrom) return;
    g_returnedAt = 0;
    int state = player::UiState();
    if (state == 3 || state < 0) {
        LOG("endzone: back in the lobby (UI state %d)", state);
        return;
    }
    uintptr_t pc = engine::LocalPlayerController();
    uintptr_t action = engine::FindObject("/Script/FortniteUI.Default__FortAsyncAction_SetUIState", "FortAsyncAction_SetUIState");
    struct {
        uintptr_t context;
        uint8_t state, pad[3];
        uintptr_t result;
    } p{ pc, 3 /*EFortUIState::FrontEnd*/ };
    Call(action, "/Script/FortniteUI.FortAsyncAction_SetUIState.SetUIState", &p);
    if (p.result) Call(p.result, "/Script/Engine.BlueprintAsyncActionBase.Activate", nullptr);
    LOG("endzone: the lobby UI was in state %d; switched it to FrontEnd", state);
}

void __fastcall OnZoneCompleted(uintptr_t manager, void*, uint32_t) {
    // Runs inside SetMissionStatus: only record here. Called for every finished primary mission, in the lobby too.
    if (!game::g_on.endzone || g_state.endAt || g_state.stage != kIdle || !IsZone()) return;
    uintptr_t gameState = engine::GameState();
    uint8_t gameplay = gameState ? At<uint8_t>(gameState, GameStateGameplayState) : 0;
    if (gameplay == 2 || gameplay == 4) return;
    uintptr_t missions = At<uint32_t>(manager, MissionManagerMissions);
    int32_t count = At<int32_t>(manager, MissionManagerMissions + 4);
    for (int32_t i = 0; missions && i < count && i < 16; i++) {
        uintptr_t mission = At<uint32_t>(missions, i * 4);
        uintptr_t info = mission ? At<uint32_t>(mission, MissionInfo) : 0;
        uint8_t status = mission ? At<uint8_t>(mission, MissionStatus) : 0;
        if (!info || At<uint8_t>(info, MissionInfoSecondary) || (status != 2 && status != 3)) continue;
        g_state.won = status == 2;
        memcpy(g_state.guid, reinterpret_cast<const void*>(mission + MissionGuid), 16);
        g_state.world = engine::World();
        g_state.endAt = GetTickCount64() + kResultsDelayMs;
        g_active = true;
        LOG("endzone: the zone is complete (%s with %s); results in %llu s", g_state.won ? "victory" : "defeat",
            engine::Name(mission).c_str(), kResultsDelayMs / 1000);
        return;
    }
}

void __fastcall HandleRequestedGameplayAction(uintptr_t gameMode, void*, uintptr_t pc, uint32_t action) {
    action &= 0xFF;  // EFortGameplayAction: 4 LeaveZone, 5 ReturnToMainMenu, 6 QuitGame
    LOG("endzone: gameplay action %u requested by %s", action, engine::Name(pc).c_str());
    bool leave = action == 4 || action == 5 || action == 6;
    if (pc && pc != engine::LocalPlayerController()) {  // a co-op player who joined: only they leave
        if (leave) SendHome(pc);
        return;
    }
    if (g_state.stage == kResults && leave) g_state.leaveRequested = true;
}

// The zone's primary mission, if it ended: true and its result (won) and GUID.
bool PrimaryResult(uintptr_t manager, bool* won, uint8_t* guid) {
    uintptr_t missions = manager ? At<uint32_t>(manager, MissionManagerMissions) : 0;
    int32_t count = manager ? At<int32_t>(manager, MissionManagerMissions + 4) : 0;
    for (int32_t i = 0; missions && i < count && i < 16; i++) {
        uintptr_t mission = At<uint32_t>(missions, i * 4);
        uintptr_t info = mission ? At<uint32_t>(mission, MissionInfo) : 0;
        uint8_t status = mission ? At<uint8_t>(mission, MissionStatus) : 0;
        if (!info || At<uint8_t>(info, MissionInfoSecondary) || (status != 2 && status != 3)) continue;
        *won = status == 2;
        memcpy(guid, reinterpret_cast<const void*>(mission + MissionGuid), 16);
        return true;
    }
    return false;
}
}  // namespace

// A co-op player who joined: the host's results screen comes with the replicated game state (GameplayState 2); the
// results report the screen reads is this copy's own (its local player's), written here from the replicated mission
// as soon as it ended (the screen picks Victory or Defeat the moment GameplayState arrives, a few seconds later).
void ClientCheck() {
    uintptr_t world = engine::World(), gameState = engine::GameState(), pc = engine::LocalPlayerController();
    if (!pc || world == g_clientReportWorld || !engine::IsA(gameState, "FortGameStateZone")) return;
    bool won = false;
    uint8_t guid[16] = {};
    if (!PrimaryResult(At<uint32_t>(gameState, GameStateMissionManager), &won, guid)) return;
    g_clientReportWorld = world;
    WriteRewardReport(pc, won, guid);
    LOG("endzone: the host's zone ended (%s); the results report is written", won ? "victory" : "defeat");
}

// A co-op player who joined is back from the host's zone: the lobby UI gets the same check as after a local zone.
void ClientReturned(uintptr_t zoneWorld) {
    g_returnedAt = GetTickCount64();
    g_returnedFrom = zoneWorld;
    g_active = true;
}

bool Install() {
    bool completed = engine::PatchPointer(kZoneCompletedEntry, kZoneCompletedStub, reinterpret_cast<void*>(&OnZoneCompleted));
    bool action = engine::PatchPointer(kGameplayActionEntry, kGameplayActionStub, reinterpret_cast<void*>(&HandleRequestedGameplayAction));
    LOG("endzone: zone-complete %s, gameplay actions %s", completed ? "hooked" : "NOT hooked", action ? "hooked" : "NOT hooked");
    return completed && action;
}

void Reset() {
    g_state = State{};
    g_active = g_returnedAt != 0;
}

void OnHudZoneCompleted() {
    g_state.hudShown = true;
}

void OnWorldTick() {
    if (!g_active) return;
    ULONGLONG now = GetTickCount64();
    if (g_returnedAt && g_state.stage == kIdle) CheckFrontend(now);
    switch (g_state.stage) {
    case kIdle:
        if (g_state.endAt && now >= g_state.endAt) ShowResults(now);
        break;
    case kResults:
        if (game::g_on.results && !g_state.hudShown && !g_state.hudFallbackDone && now - g_state.resultsAt > kHudFallbackMs) {
            g_state.hudFallbackDone = true;
            HudFallback();
        }
        if (!g_state.leaveRequested && ClickedOnResults(now)) {
            g_state.leaveRequested = true;
            LOG("endzone: clicked on the results; returning now");
        }
        if (g_state.leaveRequested) {
            Leave(now);
        } else if (now >= g_state.nextSecond) {
            g_state.nextSecond = now + 1000;
            int32_t left = --g_state.secondsLeft;
            uintptr_t gameState = engine::GameState();
            if (engine::IsA(gameState, "FortGameStateZone")) {
                At<int32_t>(gameState, GameStateVoteTimeLeft) = left;
                At<int32_t>(gameState, GameStateLeaveTimeLeft) = left;
            }
            if (left <= 0) Leave(now);
        }
        break;
    case kLeaving:
        if (!g_state.watchdogDone && now - g_state.leaveAt > kWatchdogMs && engine::World() == g_state.world) {
            g_state.watchdogDone = true;
            Watchdog();
        }
        break;
    }
    if (g_state.stage == kIdle && !g_state.endAt && !g_returnedAt) g_active = false;
}

void Finish(bool won) {
    if (!IsZone()) {
        LOG("endzone: not in a zone");
        return;
    }
    g_state.won = won;
    g_state.world = engine::World();
    g_state.endAt = GetTickCount64();
    g_state.stage = kIdle;
    g_active = true;
}

void RequestLeave() {
    if (g_state.stage == kResults) g_state.leaveRequested = true;
    else if (IsZone()) {
        g_state.world = engine::World();
        g_active = true;
        Leave(GetTickCount64());
    }
}

}  // namespace endzone
