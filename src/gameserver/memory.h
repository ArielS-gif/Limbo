#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

namespace mem {

// Caches the committed, readable regions of the process so pointer checks don't need VirtualQuery each time.
// Not thread-safe: after startup, only the game thread (inside the ProcessEvent hook) reads through it.
void Snapshot();
bool Readable(uintptr_t addr, size_t len);

// Copies with SEH protection; returns false if the memory is not readable or faults.
bool Copy(void* dst, uintptr_t src, size_t len);

template <typename T> bool Read(uintptr_t addr, T& out) {
    return Readable(addr, sizeof(T)) && Copy(&out, addr, sizeof(T));
}
template <typename T> T ReadOr(uintptr_t addr, T fallback = T{}) {
    T v;
    return Read(addr, v) ? v : fallback;
}

struct Section {
    char name[9];
    uintptr_t base;
    size_t size;
    DWORD characteristics;
};
std::vector<Section> ModuleSections(HMODULE module);

}  // namespace mem
