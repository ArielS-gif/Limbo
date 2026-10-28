// gameserver.dll: locates the engine's name and object tables in the OT6.5 client (step 1), hooks ProcessEvent to
// watch what the game does when it loads a zone (step 2; see trace.h for the commands), and puts back the
// compiled-out server pieces a zone's mission needs (step 4; game.h).
#include <windows.h>
#include <string>
#include "dump.h"
#include "engine.h"
#include "fixes.h"
#include "game.h"
#include "log.h"
#include "memory.h"
#include "mission.h"
#include "trace.h"
#include "ue.h"

static std::wstring g_dir;  // folder of this DLL, with trailing backslash

// Several copies of the game can run at once (co-op tests): the first keeps out\gameserver.log and command.txt, the
// next ones get out\instance2\ and so on. A named mutex held for the process's life claims each number.
static std::wstring InstanceDir() {
    for (int n = 1; n <= 8; n++) {
        std::wstring name = L"Local\\OTGameServer-instance-" + std::to_wstring(n);
        HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex) continue;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mutex);
            continue;
        }
        if (n == 1) return g_dir;
        std::wstring dir = g_dir + L"instance" + std::to_wstring(n) + L"\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return g_dir;
}

// Access violations in the game's own code are logged with file VAs (the shipping client writes no crash report,
// only "Fatal error!"): the faulting address, then values on the stack that point into the exe (return addresses,
// mostly), so the faulting call chain can be looked up with tools\re.js.
static LONG CALLBACK LogCrash(EXCEPTION_POINTERS* info) {
    static volatile LONG logged = 0;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || logged >= 8) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t base = ue::g.exeBase;
    auto header = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uintptr_t end = base + header->OptionalHeader.SizeOfImage;
    uintptr_t eip = info->ContextRecord->Eip;
    if (eip < base || eip >= end) return EXCEPTION_CONTINUE_SEARCH;  // probes like IsBadReadPtr fault outside the exe
    auto toFile = [base](uintptr_t va) { return static_cast<unsigned>(va - base + 0x400000); };
    InterlockedIncrement(&logged);
    char stack[16 * 10 + 1] = {};
    int found = 0;
    auto words = reinterpret_cast<const uintptr_t*>(info->ContextRecord->Esp);
    for (int i = 0; i < 2048 && found < 16; i++) {
        if (IsBadReadPtr(words + i, 4)) break;
        if (words[i] >= base + 0x1000 && words[i] < end) found += sprintf_s(stack + found * 10, 11, " %08X", toFile(words[i])) > 0;
    }
    LOG("CRASH: access violation at %08X (%s %08X), eax %08X ecx %08X edx %08X esi %08X edi %08X; stack:%s",
        toFile(eip), info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
        static_cast<unsigned>(info->ExceptionRecord->ExceptionInformation[1]), info->ContextRecord->Eax, info->ContextRecord->Ecx,
        info->ContextRecord->Edx, info->ContextRecord->Esi, info->ContextRecord->Edi, stack);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI MainThread(LPVOID) {
    g_dir = InstanceDir();
    logx::Open(g_dir + L"gameserver.log");
    LOG("gameserver.dll loaded into pid %lu", GetCurrentProcessId());

    ue::g.exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LOG("exe base 0x%08X", ue::g.exeBase);
    AddVectoredExceptionHandler(1, LogCrash);
    // Code only, so before anything else: the game logs into chat right after its own login, which can come before
    // the engine tables below exist.
    net::AllowChatLogin();
    net::AllowRememberedLogin();

    // Injected early (while the game starts), the engine tables come a little later: looked for again for a while.
    bool found = false;
    for (int attempt = 0; attempt < 90 && !found; attempt++) {
        if (attempt) Sleep(1000);
        mem::Snapshot();
        found = ue::FindNames() && ue::FindObjects();
    }
    if (!found) {
        LOG("stopping: engine tables not found");
        return 0;
    }
    dump::KnownOffsets();

    // Injected that early, the tables can exist before the game's own classes are registered (2277 objects): their
    // functions weren't found, the Play tab's hook stayed out, and Enter Outpost went to a server search that never
    // ends. The hooks wait for those classes.
    const char* const kLateFunctions[] = { "/Script/FortniteGame.FortMatchmakingContext.StartMatchmaking",
                                           "/Script/FortniteGame.FortKismetLibrary.GetAIDirector" };
    bool registered = false;
    for (int attempt = 0; attempt < 240 && !registered; attempt++) {
        if (attempt) Sleep(250);
        registered = true;
        for (const char* function : kLateFunctions) registered = registered && engine::FindFunction(function);
    }
    ue::Refresh();
    LOG("%s (%d objects)", registered ? "the game's classes are registered" : "the game's classes still aren't all there after 60 s",
        ue::g.numObjects);

    fixes::Install();
    game::Install();
    if (trace::Start(g_dir)) LOG("ready: write commands to command.txt (travel <url>, state, summary, dump)");
    return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(module, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\') + 1);
        if (HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
