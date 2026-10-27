#include "memory.h"
#include <algorithm>
#include <cstring>

namespace mem {

struct Range { uintptr_t begin, end; };
static std::vector<Range> g_ranges;
// Reads come from the game thread and from ProcessEvent on other threads; a miss inserts into g_ranges, which would
// move it under a reader on another thread.
static SRWLOCK g_rangesLock = SRWLOCK_INIT;

static bool IsReadableProtect(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void Snapshot() {
    AcquireSRWLockExclusive(&g_rangesLock);
    g_ranges.clear();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t max = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < max && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t end = begin + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && IsReadableProtect(mbi.Protect)) {
            if (!g_ranges.empty() && g_ranges.back().end == begin) g_ranges.back().end = end;  // merge neighbours
            else g_ranges.push_back({ begin, end });
        }
        if (end <= addr) break;
        addr = end;
    }
    ReleaseSRWLockExclusive(&g_rangesLock);
}

static bool Cached(uintptr_t addr, size_t len) {
    auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), addr,
                               [](uintptr_t a, const Range& r) { return a < r.begin; });
    if (it == g_ranges.begin()) return false;
    --it;
    return addr >= it->begin && addr + len <= it->end;
}

// Memory committed after the snapshot (objects and maps loaded later) is looked up on a miss and cached; any
// cached ranges it overlaps are stale and dropped. Freed ranges stay cached, but Copy() survives the fault.
static bool AddRegion(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || !IsReadableProtect(mbi.Protect)) return false;
    Range r{ reinterpret_cast<uintptr_t>(mbi.BaseAddress), reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize };
    g_ranges.erase(std::remove_if(g_ranges.begin(), g_ranges.end(),
                                  [&](const Range& x) { return x.begin < r.end && r.begin < x.end; }),
                   g_ranges.end());
    auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), r.begin,
                               [](uintptr_t a, const Range& x) { return a < x.begin; });
    g_ranges.insert(it, r);
    return true;
}

bool Readable(uintptr_t addr, size_t len) {
    if (addr < 0x10000 || addr + len < addr) return false;
    AcquireSRWLockShared(&g_rangesLock);
    bool cached = Cached(addr, len);
    ReleaseSRWLockShared(&g_rangesLock);
    if (cached) return true;
    AcquireSRWLockExclusive(&g_rangesLock);
    bool added = AddRegion(addr) && Cached(addr, len);
    ReleaseSRWLockExclusive(&g_rangesLock);
    return added;
}

bool Copy(void* dst, uintptr_t src, size_t len) {
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::vector<Section> ModuleSections(HMODULE module) {
    std::vector<Section> out;
    auto base = reinterpret_cast<uintptr_t>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        Section s{};
        memcpy(s.name, sec->Name, 8);
        s.base = base + sec->VirtualAddress;
        s.size = sec->Misc.VirtualSize;
        s.characteristics = sec->Characteristics;
        out.push_back(s);
    }
    return out;
}

}  // namespace mem
