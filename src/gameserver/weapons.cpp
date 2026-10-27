#include "engine.h"
#include "log.h"
#include "mission.h"
#include <windows.h>

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
}  // namespace

bool Install() {
    int done = 0;
    for (uint32_t entry : kEntries) done += engine::PatchPointer(entry, kPlayReloadFX, reinterpret_cast<void*>(&PlayReloadFX));
    LOG("weapons: reload and equip sounds %s", done == 2 ? "follow the player" : "NOT all hooked");
    return done == 2;
}

}  // namespace weapons
