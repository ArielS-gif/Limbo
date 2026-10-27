#include "engine.h"
#include "hook.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace atlas {

using engine::At;

namespace {
// AFortMission::SpawnAtPlacementActorsAsArray (the native behind FortMission.SpawnAtPlacementActorsAsArray, file VA
// 00785E10) is a stub in this client: an empty array and false, after which Mission_Gates_C destroys itself 2 s
// into the mission. Its only caller is the exec thunk 00A5AB50: cdecl (TArray<AActor*>* result, AFortMission*,
// bool* bSucceeded), returning `result`, whose array the thunk moves into the BP and later frees with FMemory. The
// finder it used is compiled out too, so this finds the placement actors itself and spawns each prop the way the
// surviving UFortMissionLibrary::SpawnBuildingActorAtTransform (00785F90) does.
constexpr uint32_t kSpawnAtPlacementActors = 0x00785E10;
constexpr uint8_t kStubStart[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x4C, 0x24, 0x0C };  // mov eax,[esp+4] / mov ecx,[esp+0Ch]

constexpr uint32_t kPackageFortniteGame = 0x02668A10;    // L"/Script/FortniteGame" in the exe
constexpr uint32_t kPlacementActorClass = 0x00934570;    // cdecl UClass*(const TCHAR* package): AFortPlacementActor
constexpr uint32_t kSpawnableInterface = 0x00941130;     // the same for IFortSpawnableByPlacementSystemInterface
constexpr uint32_t kGetAllActorsOfClass = 0x016B4FB0;    // cdecl (WorldContext, UClass*, TArray<AActor*>&)
constexpr uint32_t kWeakGet = 0x00DA4200;                // FWeakObjectPtr::Get(bool bEvenIfPendingKill), thiscall
constexpr uint32_t kWeakSet = 0x00D9F4C0;                // FWeakObjectPtr::operator=(const UObject*), thiscall
constexpr uint32_t kResolveAssetReference = 0x00D5AA10;  // FStringAssetReference::ResolveObject(), thiscall
constexpr uint32_t kLoadAssetReference = 0x006CE8E0;     // cdecl UObject*(const FStringAssetReference*, bool timing)
constexpr uint32_t kGetInterfaceAddress = 0x00DA4E20;    // UObject::GetInterfaceAddress(UClass*), thiscall
constexpr uint32_t kSpawnActorDeferred = 0x0076C2B0;     // UWorld::SpawnActorDeferred<ABuildingActor>, thiscall
constexpr uint32_t kFinishSpawningActor = 0x016B4770;    // cdecl AActor*(AActor*, const FTransform&)

constexpr int MissionInfo = 0x24C, MissionGuid = 0x2B4;
constexpr int InfoDistanceMin = 0xAC, InfoDistanceMax = 0xB0, InfoEntries = 0xB4;  // UFortMissionInfo.PlacementActorInfo
// FFortPlacementActorFinderEntry: BuildingToSpawn (asset pointer, path at +0xC), RequiredTags, PreferredTags,
// ExlusionTags (FGameplayTagContainer each), BuildingClassToReplace, bIgnoreCollisionCheck (used as AlwaysSpawn).
constexpr int EntrySize = 0x68, EntryPath = 0xC, EntryRequired = 0x18, EntryPreferred = 0x30, EntryExcluded = 0x48;
constexpr int EntryIgnoreCollision = 0x64;
constexpr int BuildingDefaultTags = 0x2F0, BuildingConstTags = 0x308, BuildingStaticTags = 0x320;
constexpr int BuildingOriginatingPlacementActor = 0x370;
constexpr int PlacementOccupyingActor = 0x418;
constexpr int ActorRootComponent = 0x114, ComponentToWorld = 0xD0;  // FTransform: quat, translation +0x10, scale +0x20
constexpr int WorldGameState = 0x34, GameStateMissionManager = 0x2CC;
constexpr int VtGetWorld = 0x80, VtPostSpawnInit = 0x360, VtPostInit = 0x364;
constexpr int VtSpawnableAttach = 0xC;             // ISpawnable slot 3 (mission, placement actor)
constexpr int VtRegisterForMissionEvents = 0x2E0;  // AFortMissionManager: (AActor*, FGuid by value), ret 14h

using SpawnFn = engine::TArray*(__cdecl*)(engine::TArray* result, uintptr_t mission, bool* succeeded);
SpawnFn g_original = nullptr;

uintptr_t g_goal = 0, g_goalWorld = 0;  // the last mission's first prop (the Atlas) and its world

struct Candidate {
    uintptr_t actor;
    std::vector<uint64_t> tags;  // FNames (ComparisonIndex, Number)
    float transform[12];
    bool occupied;
};

uint32_t Random() {
    static uint32_t state = GetTickCount() | 1;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// An FGameplayTagContainer holds FNames in two arrays: GameplayTags (+0) and the older Tags (+0xC).
void ReadTags(uintptr_t container, bool bothArrays, std::vector<uint64_t>& out) {
    for (int part = 0; part < (bothArrays ? 2 : 1); part++) {
        uintptr_t data = At<uint32_t>(container, part * 0xC);
        int32_t num = At<int32_t>(container, part * 0xC + 4);
        for (int32_t i = 0; data && i < num && i < 64; i++) out.push_back(At<uint64_t>(data, i * 8));
    }
}

bool Has(const std::vector<uint64_t>& owned, uint64_t tag) {
    return std::find(owned.begin(), owned.end(), tag) != owned.end();
}

bool HasAll(const std::vector<uint64_t>& owned, const std::vector<uint64_t>& tags) {
    for (uint64_t tag : tags) if (!Has(owned, tag)) return false;
    return true;
}

bool HasAny(const std::vector<uint64_t>& owned, const std::vector<uint64_t>& tags) {
    for (uint64_t tag : tags) if (Has(owned, tag)) return true;
    return false;
}

std::string TagNames(const std::vector<uint64_t>& tags) {
    std::string out;
    for (uint64_t tag : tags) out += (out.empty() ? "" : ", ") + engine::NameOf(static_cast<int32_t>(tag & 0xFFFFFFFF));
    return "[" + out + "]";
}

std::string ReadPath(uintptr_t fstring) {
    uintptr_t data = At<uint32_t>(fstring, 0);
    int32_t num = At<int32_t>(fstring, 4);
    std::string out;
    for (int32_t i = 0; data && i < num - 1 && i < 260; i++) {
        wchar_t c = At<wchar_t>(data, i * 2);
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

float Distance(const Candidate& c, const float* to) {
    float dx = c.transform[4] - to[0], dy = c.transform[5] - to[1], dz = c.transform[6] - to[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool PawnLocation(float* out) {
    uintptr_t pawn = engine::PawnOf(engine::LocalPlayerController());
    uintptr_t root = pawn ? At<uint32_t>(pawn, ActorRootComponent) : 0;
    if (!root) return false;
    memcpy(out, reinterpret_cast<const void*>(root + ComponentToWorld + 0x10), 12);
    return true;
}

std::vector<Candidate> PlacementActors(uintptr_t mission) {
    auto package = reinterpret_cast<const wchar_t*>(engine::Va(kPackageFortniteGame));
    uintptr_t placementClass = reinterpret_cast<uintptr_t(__cdecl*)(const wchar_t*)>(engine::Va(kPlacementActorClass))(package);
    engine::TArray all{};  // must start empty: GetAllActorsOfClass frees what it holds
    reinterpret_cast<void(__cdecl*)(uintptr_t, uintptr_t, engine::TArray*)>(engine::Va(kGetAllActorsOfClass))(
        mission, placementClass, &all);
    auto weakGet = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, bool)>(engine::Va(kWeakGet));
    std::vector<Candidate> out;
    for (int32_t i = 0; all.data && i < all.num; i++) {
        uintptr_t actor = At<uint32_t>(all.data, i * 4);
        if (!actor) continue;
        Candidate c{};
        c.actor = actor;
        // StaticGameplayTags already merges DefaultTags and ConstTags after PreInitializeComponents; reading all
        // three covers actors that are not that far yet.
        ReadTags(actor + BuildingStaticTags, true, c.tags);
        ReadTags(actor + BuildingDefaultTags, true, c.tags);
        ReadTags(actor + BuildingConstTags, true, c.tags);
        c.occupied = weakGet(actor + PlacementOccupyingActor, false) != 0;
        uintptr_t root = At<uint32_t>(actor, ActorRootComponent);
        if (root) memcpy(c.transform, reinterpret_cast<const void*>(root + ComponentToWorld), sizeof(c.transform));
        else { memset(c.transform, 0, sizeof(c.transform)); c.transform[3] = c.transform[8] = c.transform[9] = c.transform[10] = 1; }
        out.push_back(std::move(c));
    }
    engine::Free(reinterpret_cast<void*>(all.data));
    return out;
}

// Free placement actors with every required tag and no excluded one. If there are none, relaxes step by step and
// says how far in `level`: 1 ignores the exclusions, 2.. drops required tags from the end (always keeping the
// first), 99 takes any free placement actor.
std::vector<size_t> Match(const std::vector<Candidate>& candidates, const std::vector<uintptr_t>& used,
                          const std::vector<uint64_t>& required, const std::vector<uint64_t>& excluded, int& level) {
    auto collect = [&](const std::vector<uint64_t>& req, bool exclude) {
        std::vector<size_t> out;
        for (size_t k = 0; k < candidates.size(); k++) {
            const Candidate& c = candidates[k];
            if (c.occupied || std::find(used.begin(), used.end(), c.actor) != used.end()) continue;
            if (!HasAll(c.tags, req) || (exclude && HasAny(c.tags, excluded))) continue;
            out.push_back(k);
        }
        return out;
    };
    level = 0;
    std::vector<size_t> out = collect(required, true);
    if (!out.empty()) return out;
    level = 1;
    out = collect(required, false);
    std::vector<uint64_t> req = required;
    while (out.empty() && req.size() > 1) {
        req.pop_back();
        level++;
        out = collect(req, false);
    }
    if (out.empty()) {
        level = 99;
        out = collect({}, false);
    }
    return out;
}

// DistanceRangeMin/Max are measured from the player for the first prop and from the first prop for the others
// (a guess: the code that used them is gone), and only preferred; then a random pick, preferring PreferredTags.
size_t Choose(const std::vector<Candidate>& candidates, const std::vector<size_t>& matches,
              const std::vector<uint64_t>& preferred, const float* anchor, float minDistance, float maxDistance) {
    std::vector<size_t> inRange, farEnough;
    for (size_t k : matches) {
        float d = anchor ? Distance(candidates[k], anchor) : minDistance;
        if (d >= minDistance && d <= maxDistance) inRange.push_back(k);
        if (d >= minDistance) farEnough.push_back(k);
    }
    const std::vector<size_t>& pool = !inRange.empty() ? inRange : !farEnough.empty() ? farEnough : matches;
    std::vector<size_t> withPreferred;
    for (size_t k : pool) if (HasAny(candidates[k].tags, preferred)) withPreferred.push_back(k);
    const std::vector<size_t>& from = withPreferred.empty() ? pool : withPreferred;
    return from[Random() % from.size()];
}

uintptr_t SpawnOne(uintptr_t world, uintptr_t mission, uintptr_t cls, const Candidate& at, bool alwaysSpawn,
                   const uint32_t* guid) {
    alignas(16) float transform[12];  // SpawnActor and FinishSpawning load it with aligned SSE moves
    memcpy(transform, at.transform, sizeof(transform));
    transform[8] = transform[9] = transform[10] = 1.0f;
    transform[11] = 0.0f;

    auto deferred = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uintptr_t, const float*, uintptr_t, uintptr_t, uint32_t)>(
        engine::Va(kSpawnActorDeferred));
    uintptr_t actor = deferred(world, cls, transform, 0, 0, alwaysSpawn ? 1 : 0);
    if (!actor) return 0;

    // Before the construction script and BeginPlay run, so the prop can resolve its mission there: the spawnable
    // interface sets OriginatingPlacementActor and registers the prop for the mission's events.
    auto package = reinterpret_cast<const wchar_t*>(engine::Va(kPackageFortniteGame));
    uintptr_t interfaceClass = reinterpret_cast<uintptr_t(__cdecl*)(const wchar_t*)>(engine::Va(kSpawnableInterface))(package);
    uintptr_t spawnable = interfaceClass
        ? reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kGetInterfaceAddress))(actor, interfaceClass)
        : 0;
    if (spawnable) {
        auto attach = reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t, uintptr_t)>(
            At<uint32_t>(At<uint32_t>(spawnable, 0), VtSpawnableAttach));
        attach(spawnable, mission, at.actor);
    } else {
        At<uint32_t>(actor, BuildingOriginatingPlacementActor) = static_cast<uint32_t>(at.actor);
        uintptr_t gameState = At<uint32_t>(world, WorldGameState);
        uintptr_t manager = gameState ? At<uint32_t>(gameState, GameStateMissionManager) : 0;
        if (manager) {
            auto registerFor = reinterpret_cast<bool(__thiscall*)(uintptr_t, uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t)>(
                At<uint32_t>(At<uint32_t>(manager, 0), VtRegisterForMissionEvents));
            registerFor(manager, actor, guid[0], guid[1], guid[2], guid[3]);
        }
    }

    reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, const float*)>(engine::Va(kFinishSpawningActor))(actor, transform);
    if (!engine::IsLive(actor)) {
        LOG("atlas: %s was destroyed while spawning", engine::Name(cls).c_str());
        return 0;
    }
    uintptr_t vtable = At<uint32_t>(actor, 0);
    reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t, int32_t, uint32_t)>(At<uint32_t>(vtable, VtPostSpawnInit))(
        actor, 1 /*EFortBuildingInitializationReason::Spawned*/, -1 /*no owner persistent id*/, 0);
    reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t)>(At<uint32_t>(vtable, VtPostInit))(actor, 1);
    reinterpret_cast<void*(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kWeakSet))(at.actor + PlacementOccupyingActor, actor);
    return actor;
}

uintptr_t ResolveClass(uintptr_t entry) {
    uintptr_t reference = entry + EntryPath;
    uintptr_t cls = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t)>(engine::Va(kResolveAssetReference))(reference);
    if (!cls) cls = reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, bool)>(engine::Va(kLoadAssetReference))(reference, false);
    if (!cls || !engine::IsA(cls, "Class") || !engine::IsChildOf(cls, "BuildingActor")) {
        LOG("atlas: \"%s\" is not a building class (%s)", ReadPath(reference).c_str(), engine::Describe(cls).c_str());
        return 0;
    }
    return cls;
}

void Spawn(engine::TArray& out, uintptr_t mission, bool* succeeded) {
    std::string name = engine::Name(mission);
    uintptr_t info = At<uint32_t>(mission, MissionInfo);
    uintptr_t entries = info ? At<uint32_t>(info, InfoEntries) : 0;
    int32_t count = info ? At<int32_t>(info, InfoEntries + 4) : 0;
    if (!entries || count <= 0) {
        LOG("atlas: %s has no placement entries (MissionInfo %s)", name.c_str(), engine::Describe(info).c_str());
        return;
    }
    float minDistance = At<float>(info, InfoDistanceMin), maxDistance = At<float>(info, InfoDistanceMax);
    uint32_t guid[4];
    memcpy(guid, reinterpret_cast<const void*>(mission + MissionGuid), sizeof(guid));
    if (!(guid[0] | guid[1] | guid[2] | guid[3])) {
        // RegisterForMissionEvents ignores a zero GUID, and the props' events would never reach the mission.
        LOG("atlas: %s has no mission GUID; not spawning", name.c_str());
        return;
    }
    uintptr_t world = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t)>(At<uint32_t>(At<uint32_t>(mission, 0), VtGetWorld))(mission);
    if (!world) return;

    std::vector<Candidate> candidates = PlacementActors(mission);
    if (candidates.empty()) {
        LOG("atlas: %s: no placement actors in the world (are the tiles visible yet?)", name.c_str());
        return;
    }
    float anchor[3];
    bool hasAnchor = PawnLocation(anchor);
    LOG("atlas: %s wants %d prop(s), %.0f-%.0f from %s; %zu placement actors", name.c_str(), count, minDistance,
        maxDistance, hasAnchor ? "the player" : "anywhere (no player yet)", candidates.size());

    std::vector<uintptr_t> used;
    int spawned = 0;
    for (int32_t i = 0; i < count; i++) {
        uintptr_t entry = entries + static_cast<uintptr_t>(i) * EntrySize;
        uintptr_t cls = ResolveClass(entry);
        if (!cls) continue;
        // GameplayTags first, so relaxing drops anything from the older array before the real requirements.
        std::vector<uint64_t> required, preferred, excluded;
        ReadTags(entry + EntryRequired, true, required);
        ReadTags(entry + EntryPreferred, true, preferred);
        ReadTags(entry + EntryExcluded, true, excluded);
        int level = 0;
        std::vector<size_t> matches = Match(candidates, used, required, excluded, level);
        if (matches.empty()) {
            LOG("atlas:   [%d] %s: no free placement actor at all", i, engine::Name(cls).c_str());
            continue;
        }
        const Candidate& at = candidates[Choose(candidates, matches, preferred, hasAnchor ? anchor : nullptr,
                                                minDistance, maxDistance)];
        uintptr_t actor = SpawnOne(world, mission, cls, at, At<uint8_t>(entry, EntryIgnoreCollision) != 0, guid);
        LOG("atlas:   [%d] %s needs %s: %zu match%s (relax level %d); at %s (%.0f, %.0f, %.0f)%s -> %s", i,
            engine::Name(cls).c_str(), TagNames(required).c_str(), matches.size(), matches.size() == 1 ? "" : "es",
            level, engine::Name(at.actor).c_str(), at.transform[4], at.transform[5], at.transform[6],
            hasAnchor ? (", " + std::to_string(static_cast<int>(Distance(at, anchor))) + " from the anchor").c_str() : "",
            engine::Describe(actor).c_str());
        if (!actor) continue;
        engine::Append(out, &actor, 4);
        used.push_back(at.actor);
        if (i == 0) {
            memcpy(anchor, &at.transform[4], 12);
            hasAnchor = true;
            g_goal = actor;
            g_goalWorld = world;
        }
        spawned++;
    }
    if (succeeded) *succeeded = spawned == count;
    LOG("atlas: %s: spawned %d of %d", name.c_str(), spawned, count);
}

engine::TArray* __cdecl SpawnAtPlacementActorsHook(engine::TArray* result, uintptr_t mission, bool* succeeded) {
    result->data = 0;  // the caller's array is uninitialised stack memory
    result->num = result->max = 0;
    if (succeeded) *succeeded = false;
    if (game::g_on.atlas && mission) Spawn(*result, mission, succeeded);
    return result;
}
}  // namespace

bool Install() {
    if (!hook::Install(engine::Va(kSpawnAtPlacementActors), kStubStart, sizeof(kStubStart),
                       reinterpret_cast<void*>(&SpawnAtPlacementActorsHook), reinterpret_cast<void**>(&g_original))) {
        LOG("atlas: SpawnAtPlacementActorsAsArray NOT hooked (unexpected bytes at file VA %08X)", kSpawnAtPlacementActors);
        return false;
    }
    LOG("atlas: SpawnAtPlacementActorsAsArray hooked");
    return true;
}

uintptr_t Goal() {
    return g_goal && g_goalWorld == engine::World() && engine::IsLive(g_goal) ? g_goal : 0;
}

}  // namespace atlas
