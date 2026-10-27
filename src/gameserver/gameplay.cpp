#include "engine.h"
#include "log.h"
#include "mission.h"
#include <cstring>

namespace gameplay {

using engine::At;

namespace {
// Editing (G) asks the targeted piece whether it can be edited before sending ServerAttemptInteract with the edit
// interaction, and the server asks again: ABuildingSMActor::CanBeEdited (vtable +0x47C, 00655D70) in the classes of
// walls, floors, stairs and roofs. It refuses while the piece's build animation plays (CurBuildingAnimType) and while
// bUnderConstruction (bit 6 of +0x89C) is set; that bit is cleared by server code that is compiled out, so every placed
// piece stayed uneditable, and the player wants to edit pieces still going up anyway: both are hidden from it. Other
// refusals are logged with the parts it checks, once per piece.
constexpr uint32_t kCanBeEdited = 0x00655D70;
constexpr uint32_t kCanBeEditedEntries[] = { 0x026BB780 /*BuildingSMActor*/, 0x026B7D44 /*BuildingFloor*/, 0x026B8340 /*BuildingStairs*/,
                                             0x026B9540 /*BuildingRoof*/, 0x026BA2AC /*BuildingWall*/ };
// ABuildingSMActor: bIsPlayerBuildable (bit 3), EditModePatternData, EditModeSupport, CurBuildingAnimType, EditingPlayer,
// AttachmentPlacementBlockingActor, the bit field holding bUnderConstruction, and bit 0 of the destroyed flags.
constexpr int BuildingFlags = 0x348, BuildingPlayerBuildable = 0x8, BuildingEditPattern = 0x6C0, BuildingEditSupport = 0x6EC;
constexpr int BuildingAnimType = 0x6FA, BuildingEditingPlayer = 0x7A8, BuildingPlacementBlocker = 0x808, BuildingBits = 0x89C;
constexpr uint8_t BuildingUnderConstruction = 0x40;
constexpr int BuildingStateBits = 0x33C;

// Damage from the environment (falls, and blueprints' ApplyGlobalEnvironmentDamageToActor) goes through a helper that
// is compiled out (00720C70 is only the tag container's destructor): (AActor* target, float damage, FGameplayTagContainer
// tags by value, TSubclassOf<UGameplayEffect> template), cdecl. The landing code (AFortPawn, 00820431) computes the fall
// damage from the pawn stats' FallingDamage curve and calls it with UFortGameData.FallingDamageTemplateGE
// (GET_FallingDamage); the other caller is FortKismetLibrary.ApplyGlobalEnvironmentDamageToActor's exec (00A1F8DD).
// The templates run FortDamageFormulaExecutionCalculation with scoped modifiers that override the source's
// OutgoingBaseDamage with a ScalableFloat: the damage goes there for the one application.
constexpr uint32_t kEnvironmentDamage = 0x00720C70;
constexpr uint32_t kEnvironmentDamageCalls[] = { 0x00820431, 0x00A1F8DD };
constexpr uint32_t kAbilitySystemOf = 0x01F599F0;    // UAbilitySystemGlobals::GetAbilitySystemComponentFromActor, cdecl
constexpr uint32_t kMakeEffectContext = 0x01F696E0;  // UAbilitySystemComponent (FGameplayEffectContextHandle* out), ret 4
constexpr int HandleContext = 0x4, VtAddInstigator = 0x8, EffectContextHandleSize = 0xC;
constexpr int ClassDefaultObject = 0xA8;
// UGameplayEffect.Executions (FGameplayEffectExecutionDefinition, 0x34 each: CalculationModifiers at +0x1C, each an
// FGameplayEffectExecutionScopedModifierInfo of 0x150: the captured attribute's UProperty*, ModifierOp at +0x8, then
// FGameplayEffectModifierMagnitude: its type at +0xC, the ScalableFloat's Value at +0x10).
constexpr int EffectExecutions = 0x12C, ExecutionSize = 0x34, ExecutionModifiers = 0x1C;
constexpr int ScopedModifierSize = 0x150, ScopedModifierOp = 0x8, ScopedMagnitudeType = 0xC, ScopedMagnitudeValue = 0x10;
constexpr uint8_t kModOverride = 3, kMagnitudeScalableFloat = 0;

uintptr_t g_lastRefused = 0;  // the piece last logged as not editable
int g_damageLogged = 0;       // environment damage is logged this many times (a blueprint hazard can hit every tick)
constexpr int kDamageLogLimit = 30;

bool __fastcall CanBeEdited(uintptr_t building, void*) {
    uint8_t& animation = At<uint8_t>(building, BuildingAnimType);
    uint8_t& bits = At<uint8_t>(building, BuildingBits);
    uint8_t playing = animation, constructing = bits & BuildingUnderConstruction;
    animation = 0;  // as if built: the player may edit a piece still going up
    bits &= ~BuildingUnderConstruction;
    bool editable = reinterpret_cast<bool(__thiscall*)(uintptr_t)>(engine::Va(kCanBeEdited))(building);
    animation = playing;
    bits |= constructing;
    if (!editable && building != g_lastRefused) {
        g_lastRefused = building;
        LOG("gameplay: %s can't be edited: player buildable %d, edit pattern %s, edit support %s, editing %s, blocked by %s, "
            "under construction %d, destroyed %d", engine::Name(building).c_str(), (At<uint8_t>(building, BuildingFlags) & BuildingPlayerBuildable) != 0,
            engine::Name(At<uint32_t>(building, BuildingEditPattern)).c_str(), engine::Name(At<uint32_t>(building, BuildingEditSupport)).c_str(),
            engine::Name(At<uint32_t>(building, BuildingEditingPlayer)).c_str(),
            engine::Name(At<uint32_t>(building, BuildingPlacementBlocker)).c_str(), (At<uint8_t>(building, BuildingBits) & BuildingUnderConstruction) != 0,
            At<uint8_t>(building, BuildingStateBits) & 1);
    }
    return editable;
}

struct TagContainer {  // FGameplayTagContainer: GameplayTags, ParentTags
    engine::TArray tags, parents;
};

// The template's scoped modifier that overrides the source's OutgoingBaseDamage with a plain value, or 0.
uintptr_t DamageOverride(uintptr_t effect) {
    uintptr_t outgoing = engine::FindPropertyOn(engine::FindClass("/Script/FortniteGame.FortDamageSet"), "OutgoingBaseDamage");
    engine::TArray& executions = At<engine::TArray>(effect, EffectExecutions);
    for (int32_t e = 0; outgoing && executions.data && e < executions.num; e++) {
        engine::TArray& modifiers = At<engine::TArray>(executions.data + static_cast<uintptr_t>(e) * ExecutionSize, ExecutionModifiers);
        for (int32_t m = 0; modifiers.data && m < modifiers.num; m++) {
            uintptr_t modifier = modifiers.data + static_cast<uintptr_t>(m) * ScopedModifierSize;
            if (At<uint32_t>(modifier, 0) == outgoing && At<uint8_t>(modifier, ScopedModifierOp) == kModOverride &&
                At<uint8_t>(modifier, ScopedMagnitudeType) == kMagnitudeScalableFloat)
                return modifier;
        }
    }
    return 0;
}

void __cdecl EnvironmentDamage(uintptr_t target, float damage, TagContainer tags, uintptr_t effectClass) {
    uintptr_t abilitySystem = target ? reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t)>(engine::Va(kAbilitySystemOf))(target) : 0;
    uintptr_t effect = effectClass ? At<uint32_t>(effectClass, ClassDefaultObject) : 0;
    uintptr_t modifier = effect ? DamageOverride(effect) : 0;
    uintptr_t apply = engine::FindFunction("/Script/GameplayAbilities.AbilitySystemComponent.BP_ApplyGameplayEffectToSelf");
    if (abilitySystem && modifier && apply && damage > 0.0f) {
        struct {
            uintptr_t effectClass;
            float level;
            uint8_t context[EffectContextHandleSize];
            int32_t handle;
        } p{ effectClass, 1.0f };
        // The formula reads its source's damage attributes: the target is its own source here, named as instigator
        // (the player state the ability system's own context names leads to no ability system).
        reinterpret_cast<void*(__thiscall*)(uintptr_t, void*)>(engine::Va(kMakeEffectContext))(abilitySystem, p.context);
        uintptr_t context = *reinterpret_cast<uint32_t*>(p.context + HandleContext);
        if (context)
            reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t, uintptr_t)>(At<uint32_t>(At<uint32_t>(context, 0), VtAddInstigator))(
                context, target, target);
        float& value = At<float>(modifier, ScopedMagnitudeValue);
        float saved = value;
        value = damage;
        engine::ProcessEvent(abilitySystem, apply, &p);  // instant: the formula runs during the application
        value = saved;
        if (g_damageLogged < kDamageLogLimit && ++g_damageLogged)
            LOG("gameplay: %s takes %.0f environment damage (%s)", engine::Name(target).c_str(), damage, engine::Name(effectClass).c_str());
    } else if (damage > 0.0f && g_damageLogged < kDamageLogLimit && ++g_damageLogged) {
        LOG("gameplay: no environment damage for %s (%s: ability system %s, damage override %s)", engine::Describe(target).c_str(),
            engine::Name(effectClass).c_str(), abilitySystem ? "yes" : "no", modifier ? "yes" : "no");
    }
    if (tags.tags.data) engine::Free(reinterpret_cast<void*>(tags.tags.data));  // what the stub did: the container is ours
    if (tags.parents.data) engine::Free(reinterpret_cast<void*>(tags.parents.data));
}
}  // namespace

bool Install() {
    int edits = 0;
    for (uint32_t entry : kCanBeEditedEntries) edits += engine::PatchPointer(entry, kCanBeEdited, reinterpret_cast<void*>(&CanBeEdited));
    LOG("gameplay: %d of %zu building classes can be edited while going up", edits, std::size(kCanBeEditedEntries));
    return edits == static_cast<int>(std::size(kCanBeEditedEntries));
}

void PatchCallSites() {
    int damage = 0;
    for (uint32_t site : kEnvironmentDamageCalls) damage += engine::PatchCall(site, kEnvironmentDamage, reinterpret_cast<void*>(&EnvironmentDamage));
    LOG("gameplay: %d of %zu environment damage calls restored (falls)", damage, std::size(kEnvironmentDamageCalls));
}

}  // namespace gameplay
