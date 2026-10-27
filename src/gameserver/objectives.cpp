#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <algorithm>
#include <set>
#include <tuple>

namespace objectives {

using engine::At;

namespace {
// AFortObjectiveBase's server side is compiled out: in its vtable (02754320, shared with FortPvPObjective, _CTF and
// _3PtDomination) slots 177 ActivateObjective and 178 StartPlayingObjective point at a shared 'mov al,1; ret 4', and
// slot 190 StopObjective at a stub that only frees its by-value tag container. Only those three entries change.
constexpr uint32_t kActivateEntry = 0x027545E4, kStartPlayingEntry = 0x027545E8, kStopEntry = 0x02754618;
constexpr uint32_t kTrueStub = 0x01F64F20, kStopStub = 0x007B2140;
constexpr uint32_t kOnRepObjectiveStatus = 0x007AA810;  // AFortObjectiveBase::OnRep_ObjectiveStatus, thiscall
// AFortMission (vtable 02741E98, every mission class) slot 197 SetMissionStatus(uint8, cause), thiscall, ret 8:
// wrapped to log status changes.
constexpr uint32_t kSetMissionStatusEntry = 0x027421AC, kSetMissionStatus = 0x0077F920;
constexpr int VtHandleObjectiveStatusChanged = 0x308;  // AFortMission slot 194 (objective, status), ret 8
constexpr int VtSetMissionStatus = 0x314;
constexpr int ObjectiveStatus = 0x28C, ActorOwner = 0x5C;
constexpr int MissionStatus = 0x280, MissionActiveObjectives = 0x274;
constexpr int EventFocus = 0x20, EventInstigator = 0x28;  // FFortMissionEvent
constexpr int WorldGameState = 0x34, GameStateMissionManager = 0x2CC, MissionManagerMissions = 0x268;

// K2_RemoveItemFromPlayer's implementation (007341E0) ends in a 'return 0' stub, so a BluGlo deposit never counts.
// Only the call in its exec thunk is redirected: 007341E0 also serves the item collectors' deposits.
constexpr uint32_t kDepositCall = 0x00A41042, kRemoveItem = 0x007341E0;
constexpr uint32_t kCountItem = 0x007340E0;  // cdecl int(PC, ItemDef): K2_DoesPlayerHaveItem's implementation

const char* const kStatus[] = { "created", "in progress", "succeeded", "failed", "neutral" };

const char* StatusName(uint8_t status) {
    return status < 5 ? kStatus[status] : "?";
}

bool Finished(uint8_t status) {
    return status >= 2 && status <= 4;
}

struct EventParams {  // BlueprintOnActivated / BlueprintOnStartPlaying
    uintptr_t focus;
    uintptr_t instigator;
};

EventParams FromEvent(uintptr_t event) {
    EventParams p{};
    if (!event) return p;
    uintptr_t focus = At<uint32_t>(event, EventFocus), instigator = At<uint32_t>(event, EventInstigator);
    if (engine::IsA(focus, "Actor")) p.focus = focus;
    if (engine::IsA(instigator, "FortPawn")) p.instigator = instigator;
    return p;
}

// BP events are found on the object's own class, where the blueprint overrides them.
void CallEvent(uintptr_t object, const char* name, void* params) {
    uintptr_t function = engine::FindFunctionOn(At<uint32_t>(object, 0xC), name);
    if (function) engine::ProcessEvent(object, function, params);
}

void OnRepStatus(uintptr_t objective) {
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(engine::Va(kOnRepObjectiveStatus))(objective);
}

// Objectives activated so far, by address and name (a later object at a recycled address gets another name number).
std::set<std::tuple<uintptr_t, int32_t, int32_t>> g_activated;

bool __fastcall ActivateObjective(uintptr_t objective, void*, uintptr_t event) {
    if (!game::g_on.objectives || At<uint8_t>(objective, ObjectiveStatus) != 0) return true;
    if (!g_activated.emplace(objective, At<int32_t>(objective, 0x10), At<int32_t>(objective, 0x14)).second) return true;
    LOG("objectives: activate %s", engine::Describe(objective).c_str());
    EventParams p = FromEvent(event);
    CallEvent(objective, "BlueprintOnActivated", &p);
    return true;
}

bool __fastcall StartPlayingObjective(uintptr_t objective, void*, uintptr_t event) {
    if (!game::g_on.objectives) return true;
    uint8_t status = At<uint8_t>(objective, ObjectiveStatus);
    if (Finished(status)) return false;
    if (status == 1) return true;
    At<uint8_t>(objective, ObjectiveStatus) = 1;
    OnRepStatus(objective);
    LOG("objectives: start %s", engine::Describe(objective).c_str());
    EventParams p = FromEvent(event);
    CallEvent(objective, "BlueprintOnStartPlaying", &p);
    return true;
}

void Stop(uintptr_t objective, uint8_t status) {
    // Already finished, also when the mission's reaction below stops it again.
    if (Finished(At<uint8_t>(objective, ObjectiveStatus))) return;
    At<uint8_t>(objective, ObjectiveStatus) = status;  // before the mission reads it
    OnRepStatus(objective);
    LOG("objectives: %s %s", StatusName(status), engine::Describe(objective).c_str());
    uint8_t param[4] = { status };
    CallEvent(objective, "BlueprintOnObjectiveEnd", param);
    uintptr_t mission = At<uint32_t>(objective, ActorOwner);
    // A finished mission is not told: a required objective failing after success would turn it into a failure.
    if (engine::IsA(mission, "FortMission") && !Finished(At<uint8_t>(mission, MissionStatus))) {
        auto changed = reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t, uint32_t)>(
            At<uint32_t>(At<uint32_t>(mission, 0), VtHandleObjectiveStatusChanged));
        changed(mission, objective, status);
    }
}

struct TagContainer {  // FGameplayTagContainer passed by value: two TArrays
    uint32_t tags, tagsNum, tagsMax;
    uint32_t names, namesNum, namesMax;
};

// void StopObjective(EFortObjectiveStatus, const FString& Description, FGameplayTagContainer CompletionTags,
// UFortBadgeItemDefinition* Badge): the callee pops 24h and owns the container copy, which is freed on every path.
void __fastcall StopObjective(uintptr_t objective, void*, uint32_t status, const void* description, TagContainer tags,
                              uintptr_t badge) {
    if (game::g_on.objectives) Stop(objective, static_cast<uint8_t>(status));
    engine::Free(reinterpret_cast<void*>(tags.names));
    engine::Free(reinterpret_cast<void*>(tags.tags));
}

void __fastcall SetMissionStatus(uintptr_t mission, void*, uint32_t status, uintptr_t cause) {
    uint8_t before = At<uint8_t>(mission, MissionStatus);
    reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t, uintptr_t)>(engine::Va(kSetMissionStatus))(mission, status, cause);
    uint8_t after = At<uint8_t>(mission, MissionStatus);
    if (after != before) {
        LOG("objectives: mission %s: %s -> %s", engine::Describe(mission).c_str(), StatusName(before), StatusName(after));
        if (after == 2) LOG("objectives: *** MISSION SUCCEEDED ***");
    }
}

int __cdecl RemoveItemFromPlayer(uintptr_t pc, uintptr_t definition, int32_t amount) {
    if (!game::g_on.objectives)
        return reinterpret_cast<int(__cdecl*)(uintptr_t, uintptr_t, int32_t)>(engine::Va(kRemoveItem))(pc, definition, amount);
    if (!pc || !definition || amount <= 0) return 0;
    int have = reinterpret_cast<int(__cdecl*)(uintptr_t, uintptr_t)>(engine::Va(kCountItem))(pc, definition);
    int taken = (std::min)(amount, have);
    int removed = taken > 0 ? player::RemoveItems(pc, definition, taken) : 0;
    LOG("objectives: deposit %d x %s: the player has %d, %d taken from the inventory", amount,
        engine::Name(definition).c_str(), have, removed);
    return taken;
}

uintptr_t MissionManager() {
    uintptr_t gameState = engine::GameState();
    return gameState ? At<uint32_t>(gameState, GameStateMissionManager) : 0;
}
}  // namespace

void Reset() {
    g_activated.clear();
}

bool Install() {
    bool ok = engine::PatchPointer(kActivateEntry, kTrueStub, reinterpret_cast<void*>(&ActivateObjective));
    ok &= engine::PatchPointer(kStartPlayingEntry, kTrueStub, reinterpret_cast<void*>(&StartPlayingObjective));
    ok &= engine::PatchPointer(kStopEntry, kStopStub, reinterpret_cast<void*>(&StopObjective));
    ok &= engine::PatchPointer(kSetMissionStatusEntry, kSetMissionStatus, reinterpret_cast<void*>(&SetMissionStatus));
    LOG("objectives: Activate/StartPlaying/StopObjective and SetMissionStatus %s", ok ? "replaced" : "NOT all replaced");
    return ok;
}

bool PatchDeposit() {
    bool ok = engine::PatchCall(kDepositCall, kRemoveItem, reinterpret_cast<void*>(&RemoveItemFromPlayer));
    LOG("objectives: K2_RemoveItemFromPlayer %s", ok ? "now removes items" : "NOT patched");
    return ok;
}

void List() {
    uintptr_t manager = MissionManager();
    uintptr_t missions = manager ? At<uint32_t>(manager, MissionManagerMissions) : 0;
    int32_t count = manager ? At<int32_t>(manager, MissionManagerMissions + 4) : 0;
    LOG("objectives: %d mission(s)", count);
    for (int32_t i = 0; missions && i < count && i < 16; i++) {
        uintptr_t mission = At<uint32_t>(missions, i * 4);
        if (!engine::IsLive(mission)) continue;
        LOG("  %s: %s", engine::Describe(mission).c_str(), StatusName(At<uint8_t>(mission, MissionStatus)));
        uintptr_t objectives = At<uint32_t>(mission, MissionActiveObjectives);
        int32_t num = At<int32_t>(mission, MissionActiveObjectives + 4);
        for (int32_t k = 0; objectives && k < num && k < 32; k++) {
            uintptr_t objective = At<uint32_t>(objectives, k * 4);
            if (engine::IsLive(objective))
                LOG("    %s: %s", engine::Name(objective).c_str(), StatusName(At<uint8_t>(objective, ObjectiveStatus)));
        }
    }
}

void ForceSuccess() {
    uintptr_t manager = MissionManager();
    uintptr_t missions = manager ? At<uint32_t>(manager, MissionManagerMissions) : 0;
    int32_t count = manager ? At<int32_t>(manager, MissionManagerMissions + 4) : 0;
    for (int32_t i = 0; missions && i < count && i < 16; i++) {
        uintptr_t mission = At<uint32_t>(missions, i * 4);
        if (!engine::IsLive(mission) || Finished(At<uint8_t>(mission, MissionStatus))) continue;
        auto set = reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t, uintptr_t)>(
            At<uint32_t>(At<uint32_t>(mission, 0), VtSetMissionStatus));
        set(mission, 2, 0);
    }
}

}  // namespace objectives
