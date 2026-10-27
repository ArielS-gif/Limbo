#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
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

// The missions that run on mission events (the outpost, Deliver the Goods, Launch the Rocket, the radar grid, the
// siphon) move on when they hear that an objective succeeded: the server's mission manager called their
// HandleMissionEvent_ObjectiveSucceeded(MissionGuid, ObjectiveHandle, ...) with the objective's handle tags, and their
// blueprint matches those against its own handles (the outpost's DeployCoreObjectiveHandle, ActivateObjectiveHandle..).
// Called the same way here; the event copies what it reads, so the objective's own container is lent to it.
constexpr int PropertyOffset = 0x3C, GuidSize = 16, TagContainerSize = 24;

void SendSucceeded(uintptr_t mission, uintptr_t objective) {
    uintptr_t handler = engine::FindFunctionOn(At<uint32_t>(mission, 0xC), "HandleMissionEvent_ObjectiveSucceeded");
    if (!handler) return;
    uintptr_t guidParam = engine::FindPropertyOn(handler, "MissionGuid"), handleParam = engine::FindPropertyOn(handler, "ObjectiveHandle");
    uintptr_t guid = engine::FindPropertyOn(At<uint32_t>(mission, 0xC), "MissionGuid");
    uintptr_t handle = engine::FindPropertyOn(At<uint32_t>(objective, 0xC), "ObjectiveHandle");
    if (!guidParam || !handleParam || !guid || !handle) {
        LOG("objectives: %s not told that %s succeeded (its parameters weren't found)", engine::Name(mission).c_str(),
            engine::Name(objective).c_str());
        return;
    }
    alignas(8) uint8_t params[0x200] = {};
    memcpy(params + At<int32_t>(guidParam, PropertyOffset), reinterpret_cast<const void*>(mission + At<int32_t>(guid, PropertyOffset)), GuidSize);
    memcpy(params + At<int32_t>(handleParam, PropertyOffset), reinterpret_cast<const void*>(objective + At<int32_t>(handle, PropertyOffset)),
           TagContainerSize);
    LOG("objectives: %s hears that %s succeeded", engine::Name(mission).c_str(), engine::Name(objective).c_str());
    engine::ProcessEvent(mission, handler, params);
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
        if (status == 2) SendSucceeded(mission, objective);
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

void SucceedInProgress(const char* missionClass) {
    uintptr_t manager = MissionManager();
    uintptr_t missions = manager ? At<uint32_t>(manager, MissionManagerMissions) : 0;
    int32_t count = manager ? At<int32_t>(manager, MissionManagerMissions + 4) : 0;
    for (int32_t i = 0; missions && i < count && i < 16; i++) {
        uintptr_t mission = At<uint32_t>(missions, i * 4);
        if (!engine::IsLive(mission) || !engine::IsA(mission, missionClass)) continue;
        uintptr_t objectives = At<uint32_t>(mission, MissionActiveObjectives);
        int32_t num = At<int32_t>(mission, MissionActiveObjectives + 4);
        std::vector<uintptr_t> running;  // collected first: the mission's reaction changes the list
        for (int32_t k = 0; objectives && k < num && k < 32; k++) {
            uintptr_t objective = At<uint32_t>(objectives, k * 4);
            if (engine::IsLive(objective) && At<uint8_t>(objective, ObjectiveStatus) == 1) running.push_back(objective);
        }
        for (uintptr_t objective : running) Stop(objective, 2);
    }
}

namespace {
// The outpost's activate step (Mission_OutpostSetup): after the deploy the core has to be used again ("Activate to
// Defend!") to start the defense. Its prompt stayed off (the mission's own re-enable never came) and using it only ran
// the core's blueprint; the server counted the use for the step. Both are done here, for the step in progress whose
// handle tags are the mission's ActivateObjectiveHandle.
constexpr int StormshieldDefended = 0x950, StormshieldBuilt = 0x951;
constexpr int TagSize = 8;  // FGameplayTag: an FName

uintptr_t ObjectProperty(uintptr_t object, const char* name) {
    uintptr_t property = engine::FindPropertyOn(At<uint32_t>(object, 0xC), name);
    return property ? At<uint32_t>(object, At<int32_t>(property, PropertyOffset)) : 0;
}

// The tags of a container property (the TArray at its start).
bool SameTags(uintptr_t a, uintptr_t b) {
    int32_t num = At<int32_t>(a, 4);
    if (num <= 0 || num != At<int32_t>(b, 4)) return false;
    for (int32_t i = 0; i < num; i++) {
        bool found = false;
        for (int32_t k = 0; k < num && !found; k++)
            found = memcmp(reinterpret_cast<const void*>(At<uint32_t>(a, 0) + i * TagSize),
                           reinterpret_cast<const void*>(At<uint32_t>(b, 0) + k * TagSize), TagSize) == 0;
        if (!found) return false;
    }
    return true;
}

// The outpost mission's objective in progress whose handle tags are the mission's handle property of that name.
uintptr_t StepInProgress(uintptr_t* outpostMission, const char* handleName) {
    uintptr_t manager = MissionManager();
    uintptr_t missions = manager ? At<uint32_t>(manager, MissionManagerMissions) : 0;
    int32_t count = manager ? At<int32_t>(manager, MissionManagerMissions + 4) : 0;
    for (int32_t i = 0; missions && i < count && i < 16; i++) {
        uintptr_t mission = At<uint32_t>(missions, i * 4);
        if (!engine::IsLive(mission) || !engine::IsA(mission, "Mission_OutpostSetup_C")) continue;
        uintptr_t handleProperty = engine::FindPropertyOn(At<uint32_t>(mission, 0xC), handleName);
        if (!handleProperty) return 0;
        uintptr_t wanted = mission + At<int32_t>(handleProperty, PropertyOffset);
        uintptr_t objectives = At<uint32_t>(mission, MissionActiveObjectives);
        int32_t num = At<int32_t>(mission, MissionActiveObjectives + 4);
        for (int32_t k = 0; objectives && k < num && k < 32; k++) {
            uintptr_t objective = At<uint32_t>(objectives, k * 4);
            if (!engine::IsLive(objective) || At<uint8_t>(objective, ObjectiveStatus) != 1) continue;
            uintptr_t handle = engine::FindPropertyOn(At<uint32_t>(objective, 0xC), "ObjectiveHandle");
            if (handle && SameTags(objective + At<int32_t>(handle, PropertyOffset), wanted)) {
                *outpostMission = mission;
                return objective;
            }
        }
    }
    return 0;
}

uintptr_t ActivateStep(uintptr_t* outpostMission) {
    return StepInProgress(outpostMission, "ActivateObjectiveHandle");
}

uintptr_t OutpostMission() {
    uintptr_t manager = MissionManager();
    uintptr_t missions = manager ? At<uint32_t>(manager, MissionManagerMissions) : 0;
    int32_t count = manager ? At<int32_t>(manager, MissionManagerMissions + 4) : 0;
    for (int32_t i = 0; missions && i < count && i < 16; i++) {
        uintptr_t mission = At<uint32_t>(missions, i * 4);
        if (engine::IsLive(mission) && engine::IsA(mission, "Mission_OutpostSetup_C")) return mission;
    }
    return 0;
}

// A later visit: the server rebuilt the outpost as the finished legs left it, from the player's saved structures (their
// levels, which this client looks up and doesn't find). The mission reads its legs' quests itself (HasCompletedLeg1-5)
// and from then on expects the core (leg 1) and amplifiers 1-4 (legs 2-5) built and defended; only then does it turn
// the control panels on (with no outpost quest running, Ray sends the player there). Each is built and defended here
// once the mission knows its leg is done.
void RestoreFinishedLegs(uintptr_t mission) {
    static const char* const kLegs[][2] = {
        { "HasCompletedLeg1", "BP_StormShieldCore" }, { "HasCompletedLeg2", "BP_StormShieldAmp_1" },
        { "HasCompletedLeg3", "BP_StormShieldAmp_2" }, { "HasCompletedLeg4", "BP_StormShieldAmp_3" },
        { "HasCompletedLeg5", "BP_StormShieldAmp_4" },
    };
    if (!engine::IsLive(mission)) return;
    for (const auto& leg : kLegs) {
        uintptr_t done = engine::FindPropertyOn(At<uint32_t>(mission, 0xC), leg[0]);
        if (!done || !At<uint8_t>(mission, At<int32_t>(done, PropertyOffset))) continue;
        uintptr_t structure = ObjectProperty(mission, leg[1]);
        if (!engine::IsLive(structure)) continue;
        bool built = At<uint8_t>(structure, StormshieldBuilt) != 0, defended = At<uint8_t>(structure, StormshieldDefended) != 0;
        if (built && defended) continue;
        struct {
            bool flag;
            uint8_t padding[15];
        } build{ true };
        if (!built) CallEvent(structure, "EmergencySetBuildFlag", &build);
        if (!defended) CallEvent(structure, "WasDefended", nullptr);
        LOG("objectives: %s stands built and defended (%s)", engine::Name(structure).c_str(), leg[0]);
    }
}

// The defenses start their waves with StartMissionAIEncounterFromGeneratedProfile(CurrentDefenseNum), which reads the
// mission's GeneratedMissionOptions.GeneratedEncounterProfiles; the server's mission generator sized that array, one per
// MissionInfo encounter profile, and here it stayed empty, so no wave ever started (no husks, no countdown, no end). It
// gets those entries here, with each profile's index and type tags and no difficulty yet: the engine fills in the rest
// (difficulty, points, encounter options) when a defense starts, as it does when the difficulty changed.
constexpr int MissionInfoProperty = 0x24C, MissionGeneratedProfiles = 0x368 + 0x28;
constexpr int InfoEncounterProfiles = 0x1E0, InfoProfileSize = 0x3C, InfoProfileTags = 0x24;  // FFortEncounterProfile
constexpr int GeneratedProfileSize = 0x38, GeneratedProfileTags = 0x14, GeneratedProfileIndex = 0x34;
constexpr int TagContainerParents = 0xC;  // FGameplayTagContainer: GameplayTags, then ParentTags

void CopyTags(uintptr_t from, uintptr_t to) {
    for (int part : { 0, TagContainerParents }) {
        engine::TArray& target = At<engine::TArray>(to, part);
        for (int32_t i = 0; i < At<int32_t>(from, part + 4) && i < 64; i++)
            engine::Append(target, reinterpret_cast<const void*>(At<uint32_t>(from, part) + i * TagSize), TagSize);
    }
}

void AddEncounterProfiles(uintptr_t mission) {
    uintptr_t info = At<uint32_t>(mission, MissionInfoProperty);
    engine::TArray& profiles = At<engine::TArray>(mission, MissionGeneratedProfiles);
    if (!engine::IsLive(info) || profiles.num) return;
    int32_t count = At<int32_t>(info, InfoEncounterProfiles + 4);
    for (int32_t i = 0; i < count && i < 16; i++) {
        uint8_t profile[GeneratedProfileSize] = {};
        *reinterpret_cast<int32_t*>(profile + GeneratedProfileIndex) = i;
        if (!engine::Append(profiles, profile, GeneratedProfileSize)) break;
        CopyTags(At<uint32_t>(info, InfoEncounterProfiles) + i * InfoProfileSize + InfoProfileTags,
                 profiles.data + i * GeneratedProfileSize + GeneratedProfileTags);
    }
    LOG("objectives: %s has %d encounter profiles for its defenses (from %s)", engine::Name(mission).c_str(), profiles.num,
        engine::Name(info).c_str());
}
}  // namespace

// The outpost mission works on the structures placed in the outpost's tiles through its own references to them
// (BP_StormShieldCore, the five amplifiers, Console1-4 for the control panels, the fabricator, harvesting optimizer,
// crafting table and storage vault), which the server's mission generator filled in; here they stayed null, so the
// mission never reached its core (no prompt, no defended state). They are filled in here, each from the live actors of
// its class in a fixed order.
void LinkOutpost() {
    static const struct {
        const char* property;
        const char* cls;
        int index;
    } kLinks[] = {
        { "BP_StormShieldCore", "BP_StormShield_Core_C", 0 },
        { "BP_StormShieldAmp_1", "BP_StormShield_Amplifier_C", 0 }, { "BP_StormShieldAmp_2", "BP_StormShield_Amplifier_C", 1 },
        { "BP_StormShieldAmp_3", "BP_StormShield_Amplifier_C", 2 }, { "BP_StormShieldAmp_4", "BP_StormShield_Amplifier_C", 3 },
        { "BP_StormShieldAmp_5", "BP_StormShield_Amplifier_C", 4 },
        { "Console1", "BP_Outpost_ControlPanel_C", 0 }, { "Console2", "BP_Outpost_ControlPanel_C", 1 },
        { "Console3", "BP_Outpost_ControlPanel_C", 2 }, { "Console4", "BP_Outpost_ControlPanel_C", 3 },
        { "Fabricator", "BP_Fabricator_C", 0 }, { "HarvestingOptimizer", "BP_HarvestingOptimizer_C", 0 },
        { "CraftingTable", "BP_CraftingTable_C", 0 }, { "StorageVault", "BP_StorageVault_C", 0 },
    };
    std::vector<std::string> classes;
    for (const auto& link : kLinks)
        if (std::find(classes.begin(), classes.end(), link.cls) == classes.end()) classes.push_back(link.cls);
    std::vector<std::vector<uintptr_t>> actors = engine::FindAllOf(classes);  // one pass: a scan per class took seconds
    for (auto& ofClass : actors)
        std::sort(ofClass.begin(), ofClass.end(), [](uintptr_t a, uintptr_t b) { return engine::Name(a) < engine::Name(b); });
    uintptr_t manager = MissionManager();
    uintptr_t missions = manager ? At<uint32_t>(manager, MissionManagerMissions) : 0;
    int32_t count = manager ? At<int32_t>(manager, MissionManagerMissions + 4) : 0;
    for (int32_t i = 0; missions && i < count && i < 16; i++) {
        uintptr_t mission = At<uint32_t>(missions, i * 4);
        if (!engine::IsLive(mission) || !engine::IsA(mission, "Mission_OutpostSetup_C")) continue;
        std::string linked, missing;
        for (const auto& link : kLinks) {
            uintptr_t property = engine::FindPropertyOn(At<uint32_t>(mission, 0xC), link.property);
            if (!property || At<uint32_t>(mission, At<int32_t>(property, PropertyOffset))) continue;
            const auto& ofClass = actors[std::find(classes.begin(), classes.end(), link.cls) - classes.begin()];
            if (link.index >= static_cast<int>(ofClass.size())) {
                missing += std::string(" ") + link.property;
                continue;
            }
            At<uint32_t>(mission, At<int32_t>(property, PropertyOffset)) = static_cast<uint32_t>(ofClass[link.index]);
            linked += std::string(" ") + link.property;
        }
        if (!linked.empty() || !missing.empty())
            LOG("objectives: %s linked to its structures:%s%s%s", engine::Name(mission).c_str(), linked.c_str(),
                missing.empty() ? "" : "; none for", missing.c_str());
        AddEncounterProfiles(mission);
    }
}

uintptr_t OutpostCore() {
    uintptr_t mission = OutpostMission();
    uintptr_t core = mission ? ObjectProperty(mission, "BP_StormShieldCore") : 0;
    return engine::IsLive(core) ? core : 0;
}

void OutpostTick() {
    static std::string lastWhy;
    auto why = [](const std::string& reason) {
        if (reason != lastWhy) LOG("objectives: outpost check: %s", reason.c_str());
        lastWhy = reason;
    };
    RestoreFinishedLegs(OutpostMission());
    uintptr_t mission = 0;
    // The core deployed before its step started (a co-op player, or the same moment): the step counts it.
    if (uintptr_t deploy = StepInProgress(&mission, "DeployCoreObjectiveHandle")) {
        uintptr_t deployed = ObjectProperty(mission, "BP_StormShieldCore");
        if (engine::IsLive(deployed) && At<uint8_t>(deployed, StormshieldBuilt)) {
            LOG("objectives: %s stands already: the deploy step succeeds", engine::Name(deployed).c_str());
            Stop(deploy, 2);
        }
    }
    if (!ActivateStep(&mission)) return why("no activate step in progress");
    uintptr_t core = ObjectProperty(mission, "BP_StormShieldCore");
    if (!engine::IsLive(core)) return why("the mission has no core");
    if (!At<uint8_t>(core, StormshieldBuilt) || At<uint8_t>(core, StormshieldDefended)) return why("the core isn't built, or is defended");
    uintptr_t canInteract = engine::FindPropertyOn(At<uint32_t>(core, 0xC), "CanInteract");
    if (!canInteract) return why("the core has no CanInteract");
    if (At<uint8_t>(core, At<int32_t>(canInteract, PropertyOffset))) return why("the core's prompt is on");
    CallEvent(core, "EnableInteract", nullptr);
    LOG("objectives: %s can be activated now", engine::Name(core).c_str());
}

void OnStormshieldUsed(uintptr_t structure) {
    uintptr_t mission = 0;
    uintptr_t step = ActivateStep(&mission);
    if (!step || !engine::IsLive(structure) || !At<uint8_t>(structure, StormshieldBuilt) || At<uint8_t>(structure, StormshieldDefended))
        return;
    LOG("objectives: %s activated", engine::Name(structure).c_str());
    Stop(step, 2);
}

}  // namespace objectives
