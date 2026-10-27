// Injects gameserver.dll into the running OT6.5 client.
// Only targets FortniteClient-Win64-Shipping.exe running from the OT6.5 build folder, and only 32-bit processes,
// so it can never touch the current (live) Fortnite.
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

static const wchar_t* kProcessName = L"FortniteClient-Win64-Shipping.exe";
static const wchar_t* kRequiredPathPart = L"\\Fortnite (2252016-3240987)\\";

static std::wstring ProcessImagePath(HANDLE process) {
    wchar_t path[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(path);
    if (!QueryFullProcessImageNameW(process, 0, path, &size)) return L"";
    return std::wstring(path, size);
}

static std::vector<DWORD> FindCandidates() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry{ sizeof(entry) };
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, kProcessName) == 0) pids.push_back(entry.th32ProcessID);
    }
    CloseHandle(snap);
    return pids;
}

static bool Inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                 PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) { wprintf(L"  OpenProcess failed (%lu)\n", GetLastError()); return false; }

    std::wstring image = ProcessImagePath(process);
    BOOL wow64 = FALSE;
    IsWow64Process(process, &wow64);
    if (image.find(kRequiredPathPart) == std::wstring::npos || !wow64) {
        wprintf(L"  skipping pid %lu (not the 32-bit OT6.5 client): %s\n", pid, image.c_str());
        CloseHandle(process);
        return false;
    }

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        wprintf(L"  writing DLL path failed (%lu)\n", GetLastError());
        CloseHandle(process);
        return false;
    }

    // This injector is built 32-bit, so its kernel32!LoadLibraryW address is valid in the 32-bit target too.
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        wprintf(L"  CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 15000);
    DWORD moduleBase = 0;
    GetExitCodeThread(thread, &moduleBase);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    if (!moduleBase) { wprintf(L"  LoadLibraryW returned NULL (DLL failed to load)\n"); return false; }
    wprintf(L"  injected into pid %lu, module at 0x%08lX\n", pid, moduleBase);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dll = self;
    dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"gameserver.dll";
    DWORD onlyPid = 0;  // --pid <n>: just that process (co-op tests run several copies)
    DWORD waitSeconds = 0;  // --wait [s]: for a copy of the game that starts after this (60 s by default)
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) onlyPid = wcstoul(argv[++i], nullptr, 10);
        else if (wcscmp(argv[i], L"--wait") == 0) waitSeconds = i + 1 < argc && iswdigit(argv[i + 1][0]) ? wcstoul(argv[++i], nullptr, 10) : 60;
        else dll = argv[i];
    }

    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"DLL not found: %s\n", dll.c_str());
        return 1;
    }

    // --wait: the launcher has just been started; the game is the first new process, loaded into a second after it
    // appears (the DLL waits for the engine by itself), before the game logs in.
    if (waitSeconds && !onlyPid) {
        std::vector<DWORD> before = FindCandidates();
        wprintf(L"Waiting for %s to start...\n", kProcessName);
        for (DWORD waited = 0; !onlyPid && waited < waitSeconds * 1000; waited += 250) {
            for (DWORD pid : FindCandidates())
                if (std::find(before.begin(), before.end(), pid) == before.end()) onlyPid = pid;
            if (!onlyPid) Sleep(250);
        }
        if (!onlyPid) { wprintf(L"The game didn't start within %lu s.\n", waitSeconds); return 1; }
        Sleep(1000);
    }

    std::vector<DWORD> pids = FindCandidates();
    if (pids.empty()) { wprintf(L"No %s running.\n", kProcessName); return 1; }

    int injected = 0;
    for (DWORD pid : pids) {
        if (onlyPid && pid != onlyPid) continue;
        wprintf(L"Found %s (pid %lu)\n", kProcessName, pid);
        if (Inject(pid, dll)) injected++;
    }
    return injected > 0 ? 0 : 1;
}
