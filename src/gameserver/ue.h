#pragma once
#include <cstdint>
#include <string>

// Unreal Engine 4.12 structures as laid out in the 32-bit OT6.5 client. The finder checks these against live data.
namespace ue {

// FName table: static TNameEntryArray* (TStaticIndirectArrayThreadSafeRead<FNameEntry, 2M, 16384>).
// Layout: FNameEntry** Chunks[128]; int32 NumElements; int32 NumChunks.
constexpr int kNameChunkSize = 16384;
constexpr int kNameChunkTable = 128;
// FNameEntry: int32 Index (index << 1 | bIsWide); FNameEntry* HashNext; char/wchar_t Name[1024].
constexpr int kNameEntryIndex = 0x0, kNameEntryString = 0x8;

// UObjectBase.
namespace obj {
constexpr int VTable = 0x00, Flags = 0x04, Index = 0x08, Class = 0x0C, Name = 0x10, NameNumber = 0x14, Outer = 0x18;
}
// UStruct (UField::Next at 0x1C, then SuperStruct; no base-chain array in this build, per Blaze's Dumper-7 SDK).
namespace strct {
constexpr int Super = 0x20;
}

struct Globals {
    uintptr_t exeBase = 0;
    uintptr_t namesPtrAddr = 0;    // where the static TNameEntryArray* lives (in the exe's data section)
    uintptr_t names = 0;           // the TNameEntryArray itself
    int32_t numNames = 0;
    uintptr_t objectArrayAddr = 0; // FFixedUObjectArray { FUObjectItem* Objects; int32 Max; int32 Num; } inside GUObjectArray
    uintptr_t objects = 0;         // FUObjectItem*
    int32_t objectItemSize = 0;    // 12 in this build (Object, Flags, SerialNumber), as found live and used by Alpharium
    int32_t maxObjects = 0;
    int32_t numObjects = 0;
};
extern Globals g;

bool FindNames();
bool FindObjects();
void Refresh();                              // re-reads the name and object counts (they grow as content loads)

std::string NameOf(int32_t index);           // "" if invalid
uintptr_t ObjectAt(int32_t index);           // 0 if empty
std::string ObjectName(uintptr_t object);    // FName with _N suffix
std::string PathName(uintptr_t object);      // Package.Outer.Name
std::string ClassName(uintptr_t object);
std::string FullName(uintptr_t object);      // "Class Package.Outer.Name"
bool IsA(uintptr_t object, const char* className);  // object's class or one of its parents has this name

}  // namespace ue
