#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>
#include <cmath>

namespace weapons {

using engine::At;

namespace {
// Reload and equip sounds. AFortWeapon::PlayReloadFX_Implementation(EFortReloadFXState) (vtable slot 238, thiscall,
// ret 4; AnimNotify_PlayReloadFX calls it on the pawn's CurrentWeapon, from reload and equip animations alike) plays
// ReloadSounds[stage] attached to the weapon's root component, then the BP event OnPlayReloadFX. A weapon equipped
// here keeps its actor at the world origin while its mesh rides the pawn's hand, so those sounds played out of
// earshot (fire sounds come from the muzzle and were fine). The local player's own weapons play them as 2D sounds.
constexpr uint32_t kPlayReloadFX = 0x008FACE0;
constexpr uint32_t kEntries[] = { 0x027F04BC /*FortWeapon: melee*/, 0x027F0910 /*FortWeaponRanged*/ };
constexpr uint32_t kOnPlayReloadFX = 0x00946C40;  // the BP event's thunk (weapon, stage), thiscall, ret 4
constexpr int WeaponReloadSounds = 0x294, ActorOwner = 0x5C;

bool IsLocalPlayers(uintptr_t weapon) {
    uintptr_t pc = engine::LocalPlayerController();
    uintptr_t owner = At<uint32_t>(weapon, ActorOwner);
    return pc && owner && (owner == pc || owner == engine::PawnOf(pc));
}

void __fastcall PlayReloadFX(uintptr_t weapon, void*, uint32_t stage) {
    uint8_t s = static_cast<uint8_t>(stage);
    uintptr_t sound = s < 3 ? At<uint32_t>(weapon, WeaponReloadSounds + s * 4) : 0;
    if (!sound || !IsLocalPlayers(weapon)) {
        reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t)>(engine::Va(kPlayReloadFX))(weapon, stage);
        return;
    }
    struct {
        uintptr_t context, sound;
        float volume, pitch, startTime;
        uintptr_t concurrency;
    } p{ weapon, sound, 1.0f, 1.0f, 0.0f, 0 };
    uintptr_t statics = engine::FindObject("/Script/Engine.Default__GameplayStatics", "GameplayStatics");
    engine::ProcessEvent(statics, engine::FindFunction("/Script/Engine.GameplayStatics.PlaySound2D"), &p);
    reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t)>(engine::Va(kOnPlayReloadFX))(weapon, stage);
}

// Tracers (and the shot's start) come from the weapon's GetMuzzleLocation. B_Ranged_Generic overrides it: for the
// local player in a first-person camera, or aiming a scoped weapon (bIsTargeting && UseScopeTargeting), it returns a
// point under the camera (PlayerCameraManager.TransformComponent + ScopeTargetingMuzzleOffset, 40 units down), right
// for a first-person or scope view. The view stays third-person here, the camera over the shoulder, so those tracers
// came from behind the player. An answer that far from the gun's own muzzle socket (the native implementation,
// 008F27D0: the weapon mesh's muzzle socket) is replaced by the socket; a real first-person view stays as it is.
constexpr uint32_t kMuzzleSocketLocation = 0x008F27D0;  // thiscall (weapon, FVector* out), ret 4
constexpr float kCameraMuzzleRange = 150.0f;
}  // namespace

void AfterMuzzleLocation(uintptr_t weapon, float* location) {
    if (!engine::IsA(weapon, "FortWeaponRanged")) return;
    float socket[3] = {};
    reinterpret_cast<void(__thiscall*)(uintptr_t, float*)>(engine::Va(kMuzzleSocketLocation))(weapon, socket);
    if (socket[0] == 0.0f && socket[1] == 0.0f && socket[2] == 0.0f) return;  // no weapon mesh
    float dx = location[0] - socket[0], dy = location[1] - socket[1], dz = location[2] - socket[2];
    if (dx * dx + dy * dy + dz * dz <= kCameraMuzzleRange * kCameraMuzzleRange) return;
    static int logged = 0;
    if (logged < 3) {
        logged++;
        LOG("weapons: %s's muzzle was taken from the camera (%.0f units off); the gun's muzzle socket instead",
            engine::Name(weapon).c_str(), std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    for (int k = 0; k < 3; k++) location[k] = socket[k];
}

bool Install() {
    int done = 0;
    for (uint32_t entry : kEntries) done += engine::PatchPointer(entry, kPlayReloadFX, reinterpret_cast<void*>(&PlayReloadFX));
    LOG("weapons: reload and equip sounds %s", done == 2 ? "follow the player" : "NOT all hooked");
    return done == 2;
}

}  // namespace weapons
