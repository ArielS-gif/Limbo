#include "hook.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstring>
#include <vector>

namespace hook {

// Suspends every other thread of this process into `out` (reserved beforehand: allocating while another thread
// is suspended could deadlock on a heap lock it holds).
static void SuspendOthers(std::vector<HANDLE>& out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    THREADENTRY32 entry{ sizeof(entry) };
    for (BOOL ok = Thread32First(snap, &entry); ok && out.size() < out.capacity(); ok = Thread32Next(snap, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) == static_cast<DWORD>(-1)) { CloseHandle(t); continue; }
        out.push_back(t);
    }
    CloseHandle(snap);
}

static void ResumeAll(std::vector<HANDLE>& threads) {
    for (HANDLE t : threads) { ResumeThread(t); CloseHandle(t); }
    threads.clear();
}

// True if a suspended thread stopped part-way through the bytes we are about to overwrite.
static bool AnyInside(const std::vector<HANDLE>& threads, uintptr_t begin, uintptr_t end) {
    for (HANDLE t : threads) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &ctx) && ctx.Eip > begin && ctx.Eip < end) return true;
    }
    return false;
}

static int32_t Rel32(uintptr_t from, uintptr_t to) {  // displacement of a 5-byte jmp at `from`
    return static_cast<int32_t>(to - (from + 5));
}

bool Install(uintptr_t target, const uint8_t* expected, size_t stolen, void* detour, void** original) {
    if (stolen < 5 || stolen > 16) return false;
    if (memcmp(reinterpret_cast<const void*>(target), expected, stolen) != 0) return false;

    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, stolen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, reinterpret_cast<const void*>(target), stolen);
    tramp[stolen] = 0xE9;
    int32_t back = Rel32(reinterpret_cast<uintptr_t>(tramp) + stolen, target + stolen);
    memcpy(tramp + stolen + 1, &back, 4);

    uint8_t patch[16];
    patch[0] = 0xE9;
    int32_t to = Rel32(target, reinterpret_cast<uintptr_t>(detour));
    memcpy(patch + 1, &to, 4);
    memset(patch + 5, 0x90, stolen - 5);

    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(target), stolen, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    *original = tramp;
    std::vector<HANDLE> threads;
    threads.reserve(4096);
    bool patched = false;
    for (int attempt = 0; attempt < 100 && !patched; attempt++) {
        SuspendOthers(threads);
        if (!AnyInside(threads, target, target + stolen)) {
            memcpy(reinterpret_cast<void*>(target), patch, stolen);
            patched = true;
        }
        ResumeAll(threads);
        if (!patched) Sleep(1);
    }
    VirtualProtect(reinterpret_cast<void*>(target), stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), stolen);

    if (!patched) {
        *original = nullptr;
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

}  // namespace hook
