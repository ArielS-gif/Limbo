#include "fixes.h"
#include "engine.h"
#include "hook.h"
#include "log.h"
#include "ue.h"
#include <windows.h>
#include <cstdint>
#include <set>

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

// Debug boxes: blueprints left in this build draw red wireframe boxes into the world through
// KismetSystemLibrary.DrawDebugBox (developer aids; a shipped game never showed them). Its exec thunk's one call to
// UKismetSystemLibrary::DrawDebugBox (cdecl: world context, center, extent, color, rotation, duration; 017278B0)
// is pointed here: nothing is drawn, and the first box from each kind of object is logged with who asked.
constexpr uint32_t kDrawDebugBoxCall = 0x01AF0276, kDrawDebugBox = 0x017278B0;

// The game's C++ draws through the engine's own debug helpers too (the red boxes around building pieces in the city
// zones): DrawDebugLine, DrawDebugBox, DrawDebugSphere and the rest, 17 cdecl functions from 0168CAA0 to 0169EC20, each
// opening with GEngine->GetNetMode(World). A shipped game is built without them (ENABLE_DRAW_DEBUG 0), so nothing
// depends on them; each one returns at its first instruction (mov ecx,[GEngine] = 8B, or push ebp = 55).
const struct {
    uint32_t va;
    uint8_t first;
} kDrawDebug[] = {
    { 0x0168CAA0, 0x8B }, { 0x0168DF00, 0x8B }, { 0x0168E750, 0x55 }, { 0x01690E80, 0x8B }, { 0x01691660, 0x8B },
    { 0x01691AA0, 0x55 }, { 0x01692610, 0x55 }, { 0x01692820, 0x55 }, { 0x01692B50, 0x8B }, { 0x016930B0, 0x55 },
    { 0x01694710, 0x8B }, { 0x016947C0, 0x8B }, { 0x016948B0, 0x8B }, { 0x016949C0, 0x8B }, { 0x01694DE0, 0x8B },
    { 0x01695330, 0x8B }, { 0x0169EC20, 0x8B },
};

static void __cdecl NoDebugBox(uintptr_t worldContext, float cx, float cy, float cz, float ex, float ey, float ez) {
    static std::set<uintptr_t> logged;
    uintptr_t cls = worldContext ? *reinterpret_cast<const uint32_t*>(worldContext + 0xC) : 0;
    if (logged.size() < 32 && logged.insert(cls).second)
        LOG("fixes: a debug box left out (asked for by %s, at %.0f, %.0f, %.0f, size %.0f x %.0f x %.0f)",
            engine::Describe(worldContext).c_str(), cx, cy, cz, ex * 2, ey * 2, ez * 2);
}

// Integrated graphics: the texture pool is 70% of the *dedicated* video memory (the D3D11 device setup, 00F77C9D,
// writes GTexturePoolSize, an int64 at 031D2B40), and an Intel Iris Xe reports 128 MB of it: an 89 MB pool (716 MB on a
// 3 GB card), so characters and the world stay at their blurriest (a friend's hero head "wasn't rendered right"). The
// texture streaming reads the global every update, so a pool under 400 MB is raised to that once it is set: these GPUs
// draw from the shared system memory anyway.
constexpr uint32_t kTexturePoolSize = 0x031D2B40;
constexpr int64_t kMinTexturePool = 400LL << 20;

static DWORD WINAPI RaiseTexturePool(LPVOID) {
    auto pool = reinterpret_cast<volatile int64_t*>(engine::Va(kTexturePoolSize));
    for (int i = 0; i < 600 && *pool == 0; i++) Sleep(100);  // set while the renderer starts
    int64_t was = *pool;
    if (was > 0 && was < kMinTexturePool) {
        *pool = kMinTexturePool;
        LOG("fixes: the texture pool was %lld MB (little dedicated video memory); now %lld MB", was >> 20, kMinTexturePool >> 20);
    } else {
        LOG("fixes: the texture pool is %lld MB", was >> 20);
    }
    return 0;
}

bool Install() {
    if (HANDLE t = CreateThread(nullptr, 0, RaiseTexturePool, nullptr, 0, nullptr)) CloseHandle(t);
    bool boxes = engine::PatchCall(kDrawDebugBoxCall, kDrawDebugBox, reinterpret_cast<void*>(&NoDebugBox));
    LOG("fixes: blueprints' debug boxes %s", boxes ? "are not drawn" : "NOT patched (still drawn)");
    int off = 0;
    for (const auto& draw : kDrawDebug) off += engine::PatchByte(draw.va, draw.first, 0xC3) ? 1 : 0;
    LOG("fixes: the engine's debug drawing is off (%d of %d functions)", off, static_cast<int>(sizeof(kDrawDebug) / sizeof(kDrawDebug[0])));
    if (!hook::Install(ue::g.exeBase + kGetMcpWorlds, kGetMcpWorldsStart, sizeof(kGetMcpWorldsStart),
                       reinterpret_cast<void*>(&GetMcpWorldsHook), reinterpret_cast<void**>(&g_getMcpWorlds))) {
        LOG("fixes: GetMcpWorlds hook NOT installed (unexpected bytes at exe+0x%X, or patching failed)", kGetMcpWorlds);
        return false;
    }
    LOG("fixes: GetMcpWorlds hooked at exe+0x%X", kGetMcpWorlds);
    return true;
}

}  // namespace fixes
