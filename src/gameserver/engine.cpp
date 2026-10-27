#include "engine.h"
#include "log.h"
#include "memory.h"
#include "ue.h"
#include <windows.h>
#include <cstring>
#include <map>

namespace engine {

namespace {
using ProcessEventFn = void(__fastcall*)(uintptr_t object, void* edx, uintptr_t function, void* params);
ProcessEventFn g_processEvent = nullptr;

constexpr uint32_t kGWorld = 0x03268BEC;
// UObject* __cdecl StaticLoadObject(UClass*, UObject* Outer, const TCHAR* Name, const TCHAR* Filename,
// uint32 LoadFlags, UPackageMap* Sandbox, bool bAllowObjectReconciliation)
constexpr uint32_t kStaticLoadObject = 0x00DAFBD0;
// AActor* UWorld::SpawnActor(UClass*, const FVector*, const FRotator*, const FActorSpawnParameters&), thiscall, ret 10h
constexpr uint32_t kSpawnActor = 0x01753930;
constexpr uint32_t kSpawnParametersCtor = 0x019C1100;  // FActorSpawnParameters::FActorSpawnParameters (0x20 bytes)
constexpr int kSpawnParametersOwner = 0xC, kSpawnParametersCollision = 0x18;
constexpr uint32_t kRealloc = 0x00CA6C50, kFree = 0x00C9CAD0;  // FMemory::Realloc(ptr, size, align), Free(ptr): cdecl

constexpr int WorldGameState = 0x34, WorldAuthorityGameMode = 0x94, WorldOwningGameInstance = 0xB0;
constexpr int GameInstanceLocalPlayers = 0x24, PlayerController = 0x20;
constexpr int ControllerPawn = 0x244, FortPlayerControllerMyFortPawn = 0x50C;
constexpr int StructChildren = 0x24, FieldNext = 0x1C;
constexpr uint32_t kPendingKillOrUnreachable = (1u << 29) | (1u << 28);  // FUObjectItem::Flags
}  // namespace

uintptr_t Va(uint32_t fileVa) {
    return ue::g.exeBase + (fileVa - 0x400000);
}

std::recursive_mutex& Lock() {
    static std::recursive_mutex lock;
    return lock;
}

void SetProcessEvent(void* original) {
    g_processEvent = reinterpret_cast<ProcessEventFn>(original);
}

bool Ready() {
    return g_processEvent != nullptr;
}

void ProcessEvent(uintptr_t object, uintptr_t function, void* params) {
    // A stale or wrong pointer is logged here instead of taking the game down inside the engine.
    if (!g_processEvent || !function || !IsLive(object)) {
        LOG("engine: ProcessEvent skipped (object %08X, function %08X)", static_cast<unsigned>(object),
            static_cast<unsigned>(function));
        return;
    }
    g_processEvent(object, nullptr, function, params);
}

uintptr_t World() {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    return mem::ReadOr<uint32_t>(Va(kGWorld));
}

uintptr_t GameMode() {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    uintptr_t world = World();
    return world ? mem::ReadOr<uint32_t>(world + WorldAuthorityGameMode) : 0;
}

uintptr_t GameState() {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    uintptr_t world = World();
    return world ? mem::ReadOr<uint32_t>(world + WorldGameState) : 0;
}

uintptr_t LocalPlayerController() {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    uintptr_t world = World();
    uintptr_t gameInstance = world ? mem::ReadOr<uint32_t>(world + WorldOwningGameInstance) : 0;
    if (!gameInstance) return 0;
    uintptr_t players = mem::ReadOr<uint32_t>(gameInstance + GameInstanceLocalPlayers);
    int32_t count = mem::ReadOr<int32_t>(gameInstance + GameInstanceLocalPlayers + 4);
    if (!players || count < 1) return 0;
    uintptr_t player = mem::ReadOr<uint32_t>(players);
    return player ? mem::ReadOr<uint32_t>(player + PlayerController) : 0;
}

uintptr_t PawnOf(uintptr_t controller) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    if (!controller) return 0;
    uintptr_t pawn = IsA(controller, "FortPlayerController") ? mem::ReadOr<uint32_t>(controller + FortPlayerControllerMyFortPawn) : 0;
    return pawn ? pawn : mem::ReadOr<uint32_t>(controller + ControllerPawn);
}

uintptr_t FindObject(const std::string& path, const char* className) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    static std::map<std::string, uintptr_t> found;
    auto it = found.find(path);
    if (it != found.end()) return it->second;
    ue::Refresh();
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (object && ue::ClassName(object) == className && ue::PathName(object) == path) return found[path] = object;
    }
    return 0;
}

uintptr_t FindFunction(const std::string& path) {
    return FindObject(path, "Function");
}

uintptr_t FindClass(const std::string& path) {
    return FindObject(path, "Class");
}

uintptr_t FindFunctionOn(uintptr_t cls, const char* name) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    static std::map<std::pair<uintptr_t, std::string>, uintptr_t> found;
    auto key = std::make_pair(cls, std::string(name));
    auto it = found.find(key);
    if (it != found.end()) return it->second;
    for (uintptr_t c = cls; c; c = mem::ReadOr<uint32_t>(c + ue::strct::Super)) {
        for (uintptr_t field = mem::ReadOr<uint32_t>(c + StructChildren); field;
             field = mem::ReadOr<uint32_t>(field + FieldNext)) {
            if (ue::ObjectName(field) == name && ue::ClassName(field) == "Function") return found[key] = field;
        }
    }
    return 0;
}

uintptr_t FindPropertyOn(uintptr_t cls, const char* name) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    for (uintptr_t c = cls; c; c = mem::ReadOr<uint32_t>(c + ue::strct::Super)) {
        for (uintptr_t field = mem::ReadOr<uint32_t>(c + StructChildren); field;
             field = mem::ReadOr<uint32_t>(field + FieldNext)) {
            std::string type = ue::ClassName(field);
            if (ue::ObjectName(field) == name && type.size() > 8 && type.compare(type.size() - 8, 8, "Property") == 0) return field;
        }
    }
    return 0;
}

uintptr_t Load(const std::wstring& path) {
    uintptr_t objectClass = FindClass("/Script/CoreUObject.Object");
    if (!objectClass) return 0;
    auto load = reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, uintptr_t, const wchar_t*, const wchar_t*, uint32_t,
                                                     uintptr_t, bool)>(Va(kStaticLoadObject));
    uintptr_t object = load(objectClass, 0, path.c_str(), nullptr, 0, 0, true);  // may flush loading: no lock here
    if (!object) {
        std::lock_guard<std::recursive_mutex> guard(Lock());
        LOG("engine: can't load %ls", path.c_str());
    }
    return object;
}

uintptr_t FindSubobject(uintptr_t outer, const char* className) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    ue::Refresh();
    for (int32_t i = 0; outer && i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (object && mem::ReadOr<uint32_t>(object + ue::obj::Outer) == outer && ue::ClassName(object) == className) return object;
    }
    return 0;
}

uintptr_t FindFirstOfClass(const char* className) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    ue::Refresh();
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (object && ue::ClassName(object) == className && ue::ObjectName(object).rfind("Default__", 0) != 0) return object;
    }
    return 0;
}

uintptr_t FindFirstA(const char* className) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    ue::Refresh();
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (object && ue::IsA(object, className) && ue::ObjectName(object).rfind("Default__", 0) != 0) return object;
    }
    return 0;
}

std::vector<uintptr_t> FindAllA(const char* className) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    ue::Refresh();
    std::vector<uintptr_t> found;
    for (int32_t i = 0; i < ue::g.numObjects; i++) {
        uintptr_t object = ue::ObjectAt(i);
        if (object && ue::IsA(object, className) && ue::ObjectName(object).rfind("Default__", 0) != 0) found.push_back(object);
    }
    return found;
}

uintptr_t SpawnObject(uintptr_t cls, uintptr_t outer) {
    uintptr_t statics = FindObject("/Script/Engine.Default__GameplayStatics", "GameplayStatics");
    uintptr_t function = FindFunction("/Script/Engine.GameplayStatics.SpawnObject");
    struct {
        uintptr_t cls, outer, result;
    } p{ cls, outer, 0 };
    if (statics && function && cls && outer) ProcessEvent(statics, function, &p);
    return p.result;
}

bool Location(uintptr_t actor, float* xyz) {
    constexpr int ActorRootComponent = 0x114, ComponentToWorldTranslation = 0xD0 + 0x10;
    uintptr_t root = actor ? At<uint32_t>(actor, ActorRootComponent) : 0;
    if (!root) return false;
    memcpy(xyz, reinterpret_cast<const void*>(root + ComponentToWorldTranslation), 12);
    return true;
}

bool Teleport(uintptr_t actor, const float* xyz) {
    struct {
        float location[3], rotation[3];
        bool result;
        uint8_t pad[3];
    } p{ { xyz[0], xyz[1], xyz[2] }, {}, false };
    ProcessEvent(actor, FindFunction("/Script/Engine.Actor.K2_TeleportTo"), &p);
    return p.result;
}

bool IsLive(uintptr_t object) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    if (!object || !mem::Readable(object, 0x1C)) return false;
    int32_t index = mem::ReadOr<int32_t>(object + ue::obj::Index, -1);
    if (index < 0 || index >= ue::g.maxObjects) return false;
    uintptr_t item = ue::g.objects + static_cast<uintptr_t>(index) * ue::g.objectItemSize;
    return mem::ReadOr<uint32_t>(item) == object && !(mem::ReadOr<uint32_t>(item + 4) & kPendingKillOrUnreachable);
}

bool IsA(uintptr_t object, const char* className) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    return object && ue::IsA(object, className);
}

bool IsChildOf(uintptr_t cls, const char* className) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    for (int depth = 0; cls && depth < 32; depth++) {
        if (ue::ObjectName(cls) == className) return true;
        cls = mem::ReadOr<uint32_t>(cls + ue::strct::Super);
    }
    return false;
}

std::string Name(uintptr_t object) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    return object ? ue::ObjectName(object) : "null";
}

std::string Describe(uintptr_t object) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    return object ? ue::FullName(object) : "null";
}

std::string NameOf(int32_t index) {
    std::lock_guard<std::recursive_mutex> guard(Lock());
    return ue::NameOf(index);
}

uintptr_t SpawnActor(uintptr_t cls, uintptr_t owner) {
    return SpawnActorAt(cls, owner, nullptr, nullptr);
}

uintptr_t SpawnActorAt(uintptr_t cls, uintptr_t owner, const float* location, const float* rotation, uint8_t collision) {
    uintptr_t world = World();
    if (!world || !cls) return 0;
    alignas(16) uint8_t parameters[0x20] = {};
    reinterpret_cast<void*(__thiscall*)(void*)>(Va(kSpawnParametersCtor))(parameters);
    memcpy(parameters + kSpawnParametersOwner, &owner, 4);
    parameters[kSpawnParametersCollision] = collision;
    auto spawn = reinterpret_cast<uintptr_t(__thiscall*)(uintptr_t, uintptr_t, const void*, const void*, const void*)>(
        Va(kSpawnActor));
    return spawn(world, cls, location, rotation, parameters);
}

bool PatchPointer(uint32_t entryVa, uint32_t expectedVa, void* replacement) {
    auto entry = reinterpret_cast<uint32_t*>(Va(entryVa));
    if (*entry != Va(expectedVa)) {
        LOG("engine: %08X holds %08X (file VA), not %08X: left alone", entryVa,
            static_cast<unsigned>(*entry - ue::g.exeBase + 0x400000), expectedVa);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(entry, 4, PAGE_READWRITE, &old)) return false;
    *entry = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement));
    VirtualProtect(entry, 4, old, &old);
    return true;
}

bool PatchByte(uint32_t va, uint8_t expected, uint8_t value) {
    auto at = reinterpret_cast<uint8_t*>(Va(va));
    if (*at == value) return true;
    if (*at != expected) {
        LOG("engine: %08X holds %02X, not %02X: left alone", va, *at, expected);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *at = value;
    VirtualProtect(at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    return true;
}

bool PatchCall(uint32_t siteVa, uint32_t expectedTargetVa, void* replacement) {
    auto site = reinterpret_cast<uint8_t*>(Va(siteVa));
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || reinterpret_cast<uintptr_t>(site) + 5 + rel != Va(expectedTargetVa)) {
        LOG("engine: no call to %08X at %08X: left alone", expectedTargetVa, siteVa);
        return false;
    }
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement) - (reinterpret_cast<uintptr_t>(site) + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

void* Realloc(void* data, size_t bytes) {
    return reinterpret_cast<void*(__cdecl*)(void*, size_t, uint32_t)>(Va(kRealloc))(data, bytes, 0);
}

void Free(void* data) {
    if (data) reinterpret_cast<void(__cdecl*)(void*)>(Va(kFree))(data);
}

bool Append(TArray& array, const void* element, int size) {
    if (array.num >= array.max) {
        int32_t max = array.max < 4 ? 4 : array.max * 2;
        void* data = Realloc(reinterpret_cast<void*>(array.data), static_cast<size_t>(max) * size);
        if (!data) return false;
        array.data = reinterpret_cast<uintptr_t>(data);
        array.max = max;
    }
    memcpy(reinterpret_cast<void*>(array.data + static_cast<uintptr_t>(array.num) * size), element, size);
    array.num++;
    return true;
}

}  // namespace engine
