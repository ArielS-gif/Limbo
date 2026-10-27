#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Shared access to the running engine for the tracer and the mission code. Lookups take the lock (the ue:: and
// mem:: helpers are not thread-safe); calls into the engine are made without it, on the game thread, so the engine
// can never wait on a thread that is itself waiting for the lock.
namespace engine {

// Engine addresses in the mission code are file VAs (image base 0x400000), the way tools\re.js prints them; the exe
// loads at another base at runtime.
uintptr_t Va(uint32_t fileVa);

std::recursive_mutex& Lock();

void SetProcessEvent(void* original);  // the trampoline of the ProcessEvent hook
bool Ready();                          // ProcessEvent can be called
void ProcessEvent(uintptr_t object, uintptr_t function, void* params);

uintptr_t World();
uintptr_t GameMode();
uintptr_t GameState();
uintptr_t LocalPlayerController();
uintptr_t PawnOf(uintptr_t controller);  // FortPlayerController.MyFortPawn, else Controller.Pawn

// Found objects are cached by path, misses are not: use these for native classes, functions and default objects,
// and Load for assets.
uintptr_t FindObject(const std::string& path, const char* className);
uintptr_t FindFunction(const std::string& path);
uintptr_t FindClass(const std::string& path);
uintptr_t FindFunctionOn(uintptr_t cls, const char* name);  // the most derived UFunction of that name, e.g. a BP event
uintptr_t FindPropertyOn(uintptr_t cls, const char* name);  // a UProperty of that name on the class or its supers
uintptr_t Load(const std::wstring& path);                   // StaticLoadObject; 0 if it doesn't exist
uintptr_t FindSubobject(uintptr_t outer, const char* className);  // scans GObjects
uintptr_t FindFirstOfClass(const char* className);                // a live instance, not a default object
uintptr_t FindFirstA(const char* className);                      // the same, of that class or a subclass (BP)
std::vector<uintptr_t> FindAllA(const char* className);           // every live instance of that class or a subclass
uintptr_t SpawnObject(uintptr_t cls, uintptr_t outer);            // GameplayStatics.SpawnObject

bool Location(uintptr_t actor, float* xyz);         // the root component's world location
bool Teleport(uintptr_t actor, const float* xyz);   // Actor.K2_TeleportTo, keeping no rotation

bool IsLive(uintptr_t object);  // still in GObjects and not pending kill or unreachable
bool IsA(uintptr_t object, const char* className);
bool IsChildOf(uintptr_t cls, const char* className);
std::string Name(uintptr_t object);
std::string Describe(uintptr_t object);  // "Class Package.Outer.Name", or "null"
std::string NameOf(int32_t index);

uintptr_t SpawnActor(uintptr_t cls, uintptr_t owner);  // UWorld::SpawnActor at the origin
// The same at a location (x, y, z) and rotation (pitch, yaw, roll), either of which may be null, and with the spawn's
// ESpawnActorCollisionHandlingMethod (0: the class's own, 2: moved out of what it overlaps if it can, spawned anyway).
uintptr_t SpawnActorAt(uintptr_t cls, uintptr_t owner, const float* location, const float* rotation, uint8_t collision = 0);
void* Realloc(void* data, size_t bytes);              // FMemory: whatever the engine frees must come from here
void Free(void* data);

// Code patches, each checked against what the exe should hold there. PatchPointer swaps a vtable entry (an aligned
// write); PatchCall points an E8 call at `replacement` (run it on the thread that runs that code, or before it runs).
bool PatchPointer(uint32_t entryVa, uint32_t expectedVa, void* replacement);
bool PatchCall(uint32_t siteVa, uint32_t expectedTargetVa, void* replacement);
bool PatchByte(uint32_t va, uint8_t expected, uint8_t value);  // one byte of code (e.g. a jne made a jmp)

struct TArray {
    uintptr_t data;
    int32_t num;
    int32_t max;
};
// Appends `size` bytes to an engine TArray, growing it with FMemory.
bool Append(TArray& array, const void* element, int size);

template <typename T> T& At(uintptr_t object, int offset) {
    return *reinterpret_cast<T*>(object + offset);
}

}  // namespace engine
