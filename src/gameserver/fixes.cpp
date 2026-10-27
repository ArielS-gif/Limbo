#include "fixes.h"
#include "hook.h"
#include "log.h"
#include "ue.h"
#include <cstdint>

namespace fixes {

// UFortMcpWorlds* __cdecl GetMcpWorlds(UObject* WorldContext, const TSharedPtr<FUniqueNetId>& User) at exe+0x3CA5C0
// (file VA 007CA5C0): gets or creates the singleton (global exe+0x2D88228) and copies User into its +0x20 (object)
// and +0x24 (reference controller). Its first three instructions (push ebx / push ebp / push edi /
// push [esp+10h]) move to the trampoline.
constexpr uint32_t kGetMcpWorlds = 0x3CA5C0;
constexpr uint8_t kGetMcpWorldsStart[] = { 0x53, 0x55, 0x57, 0xFF, 0x74, 0x24, 0x10 };
constexpr uint32_t kMcpWorldsSingleton = 0x2D88228;
constexpr int kMcpWorldsUser = 0x20;

using GetMcpWorldsFn = uintptr_t(__cdecl*)(uintptr_t worldContext, const uint32_t* user);
static GetMcpWorldsFn g_getMcpWorlds = nullptr;

// Runs on the game thread without the tracer's lock, so it reads directly (the global is in the exe's data and the
// singleton is a live rooted object) instead of through mem::, whose cache is not thread-safe.
static uintptr_t __cdecl GetMcpWorldsHook(uintptr_t worldContext, const uint32_t* user) {
    if (user && user[0] == 0 && user[1] == 0) {
        uintptr_t singleton = *reinterpret_cast<const uint32_t*>(ue::g.exeBase + kMcpWorldsSingleton);
        if (singleton && *reinterpret_cast<const uint32_t*>(singleton + kMcpWorldsUser)) {
            uint32_t current[2] = { *reinterpret_cast<const uint32_t*>(singleton + kMcpWorldsUser),
                                    *reinterpret_cast<const uint32_t*>(singleton + kMcpWorldsUser + 4) };
            static bool logged = false;
            if (!logged) {
                LOG("fixes: world requests keep the logged-in user instead of the empty one (world context %08X)",
                    static_cast<unsigned>(worldContext));
                logged = true;
            }
            // Same controller as already stored: the original skips the reference counting and rewrites the
            // object pointer with the value it already has.
            return g_getMcpWorlds(worldContext, current);
        }
    }
    return g_getMcpWorlds(worldContext, user);
}

bool Install() {
    if (!hook::Install(ue::g.exeBase + kGetMcpWorlds, kGetMcpWorldsStart, sizeof(kGetMcpWorldsStart),
                       reinterpret_cast<void*>(&GetMcpWorldsHook), reinterpret_cast<void**>(&g_getMcpWorlds))) {
        LOG("fixes: GetMcpWorlds hook NOT installed (unexpected bytes at exe+0x%X, or patching failed)", kGetMcpWorlds);
        return false;
    }
    LOG("fixes: GetMcpWorlds hooked at exe+0x%X", kGetMcpWorlds);
    return true;
}

}  // namespace fixes
