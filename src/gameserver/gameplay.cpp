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

// Deploying an outpost's storm shield: the core's blueprint calls BuildingOutpostStormshieldUpgrade.Build(controller)
// (006828E0, thiscall, ret 4), which only marks the build pending (+0x952) and has the controller ask the server for the
// structure (UpgradeStructure -> ServerUpgradeStructure); the server's answer set bBuilt. Nothing answers here, so the
// core stayed unbuilt and the outpost mission waited forever. The exec thunk's call (00A21F33) is pointed here instead:
// the core is built at once through EmergencySetBuildFlag(true) (006886A0, thiscall, ret 4), which sets bBuilt and
// fires the core's built events like the server's answer did, and the outpost mission's step in progress (deploy the
// core, or install an amplifier: the same Build) succeeds, which the server's answer also brought.
constexpr uint32_t kBuildCall = 0x00A21F33, kBuild = 0x006828E0, kEmergencySetBuildFlag = 0x006886A0;
constexpr int StormshieldBuilt = 0x951;

void __fastcall BuildStormshield(uintptr_t core, void*, uintptr_t controller) {
    if (!controller || At<uint8_t>(core, StormshieldBuilt)) return;
    reinterpret_cast<void(__thiscall*)(uintptr_t, int)>(engine::Va(kEmergencySetBuildFlag))(core, 1);
    LOG("gameplay: %s deployed %s", engine::Name(controller).c_str(), engine::Name(core).c_str());
    objectives::SucceedInProgress("Mission_OutpostSetup_C");
}

// The building and editing sounds (choosing a piece or material, rotating, edit mode and its tiles, can't build) are UI
// feedback events (PlayerUIFeedback), played by 008804F0 on the local controller with GameplayStatics.
// SpawnSoundAttached(sound, the controller's root, ...) (016C3730, cdecl; called at 00880615). Their cues attenuate and
// spatialize, and the controller's root stays where it was when the zone started (attached to the camera manager in
// name only), so the farther the player went from there, the quieter they got, down to nothing. They are attached to
// the camera manager's root instead, which is where the camera (and the listener) is.
constexpr uint32_t kFeedbackSoundCall = 0x00880615, kSpawnSoundAttached = 0x016C3730;
constexpr int ActorRoot = 0x114, ControllerCameraManager = 0x2A0;
struct Vec3 {
    float x, y, z;
};
using SpawnSoundAttachedFn = uintptr_t(__cdecl*)(uintptr_t, uintptr_t, uint64_t, Vec3, Vec3, int32_t, int32_t, float, float, float,
                                                 uintptr_t, uintptr_t);

uintptr_t __cdecl FeedbackSound(uintptr_t sound, uintptr_t attachTo, uint64_t socket, Vec3 location, Vec3 rotation, int32_t locationType,
                                int32_t stopWithParent, float volume, float pitch, float start, uintptr_t attenuation, uintptr_t concurrency) {
    uintptr_t pc = engine::LocalPlayerController();
    uintptr_t cameraManager = pc ? At<uint32_t>(pc, ControllerCameraManager) : 0;
    uintptr_t camera = cameraManager ? At<uint32_t>(cameraManager, ActorRoot) : 0;
    if (camera && attachTo == At<uint32_t>(pc, ActorRoot)) attachTo = camera;
    return reinterpret_cast<SpawnSoundAttachedFn>(engine::Va(kSpawnSoundAttached))(
        sound, attachTo, socket, location, rotation, locationType, stopWithParent, volume, pitch, start, attenuation, concurrency);
}

// Blueprints drop items with FortKismetLibrary.K2_SpawnPickupInWorld (context, item, count, position, direction,
// override max stack; cdecl): the outpost's supply crate drops its wood, stone, metal and nuts and bolts that way. Its
// implementation (01372E50) is compiled out to a bare ret, so the crate burst open and nothing came out. The exec
// thunk's call (00A411E1) spawns the pickup here, where the blueprint put it (only where the zone is run, not on a
// joined player's copy: the host's pickups reach it).
constexpr uint32_t kSpawnPickupInWorldCall = 0x00A411E1, kSpawnPickupInWorld = 0x01372E50;
struct Vector {
    float x, y, z;
};
int g_dropsLogged = 0;

uintptr_t __cdecl SpawnPickupInWorld(uintptr_t context, uintptr_t definition, int32_t count, Vector position, Vector direction,
                                     int32_t) {
    if (net::IsClient()) return 0;
    float from[3] = { position.x, position.y, position.z };
    float to[3] = { position.x + direction.x, position.y + direction.y, position.z + direction.z };
    uintptr_t pickup = player::SpawnPickupAt(definition, count, from, to, 0, 0);
    if (g_dropsLogged < 24) {
        g_dropsLogged++;
        LOG("gameplay: %s drops %d x %s%s", engine::Name(context).c_str(), count, engine::Name(definition).c_str(),
            pickup ? "" : " (no pickup)");
    }
    return pickup;
}

// The outpost menu's Build (FortOutpostContext.UpgradeStructure and FortPlayerControllerOutpost.UpgradeStructure call
// AFortPlayerControllerOutpost::UpgradeStructure, 008B0D20, thiscall (structure), at 00839A00 and 00A61473): when
// CanUpgradeStructure agrees it marks the upgrade pending (+0xC88) and, on a client, asks the server
// (ServerUpgradeStructure), which took the cost, raised the structure's level and saved it. On the player who hosts
// nothing more happened. Here: the cost (GetUpgradeStructureCost: world items out of the inventory, account items paid
// by the backend), the structure item's level up, ClientOnStructureLevelChanged, and the level kept for the outpost.
constexpr uint32_t kUpgradeStructure = 0x008B0D20;
constexpr uint32_t kUpgradeStructureCalls[] = { 0x00839A00, 0x00A61473 };
constexpr int ActorRole = 0xC8, ControllerPendingUpgrade = 0xC88;
constexpr int QuantityPairSize = 0x1C, QuantityPairPath = 0xC, QuantityPairCount = 0x18;  // FFortItemQuantityPair

// A quantity pair's item: its asset path (TAssetPtr's FString), and the asset's name.
std::wstring PairPath(uintptr_t pair) {
    const wchar_t* text = reinterpret_cast<const wchar_t*>(At<uint32_t>(pair, QuantityPairPath));
    int32_t length = At<int32_t>(pair, QuantityPairPath + 4);
    return text && length > 1 ? std::wstring(text, length - 1) : L"";
}

void FreePairs(engine::TArray& pairs) {
    for (int32_t i = 0; pairs.data && i < pairs.num; i++)
        engine::Free(reinterpret_cast<void*>(At<uint32_t>(pairs.data + static_cast<uintptr_t>(i) * QuantityPairSize, QuantityPairPath)));
    engine::Free(reinterpret_cast<void*>(pairs.data));
}

void __fastcall UpgradeStructure(uintptr_t pc, void*, uintptr_t structure) {
    reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(engine::Va(kUpgradeStructure))(pc, structure);
    if (At<uint8_t>(pc, ActorRole) != 3 || At<uint32_t>(pc, ControllerPendingUpgrade) != structure) return;
    At<uint32_t>(pc, ControllerPendingUpgrade) = 0;
    int before = player::StructureLevel(pc, structure);
    struct {
        uintptr_t structure;
        engine::TArray world, account;
    } cost{ structure };
    engine::ProcessEvent(pc, engine::FindFunction("/Script/FortniteGame.FortPlayerControllerOutpost.GetUpgradeStructureCost"), &cost);
    std::string spent;
    for (int32_t i = 0; cost.world.data && i < cost.world.num; i++) {
        uintptr_t pair = cost.world.data + static_cast<uintptr_t>(i) * QuantityPairSize;
        uintptr_t item = engine::Load(PairPath(pair));
        int count = At<int32_t>(pair, QuantityPairCount);
        if (item && count > 0) spent += " " + std::to_string(player::RemoveItems(pc, item, count)) + " " + engine::Name(item);
    }
    std::vector<std::pair<std::string, int>> accountItems;
    for (int32_t i = 0; cost.account.data && i < cost.account.num; i++) {
        uintptr_t pair = cost.account.data + static_cast<uintptr_t>(i) * QuantityPairSize;
        std::wstring path = PairPath(pair);
        std::string name;
        for (size_t c = path.rfind(L'.') + 1; c < path.size(); c++) name.push_back(static_cast<char>(path[c]));  // ASCII names
        int count = At<int32_t>(pair, QuantityPairCount);
        if (!name.empty() && count > 0) {
            accountItems.emplace_back(name, count);
            spent += " " + std::to_string(count) + " " + name;
        }
    }
    FreePairs(cost.world);
    FreePairs(cost.account);
    player::SetStructureLevel(pc, structure, before + 1);
    struct {
        uintptr_t structure;
        int32_t before;
    } changed{ structure, before };
    engine::ProcessEvent(pc, engine::FindFunction("/Script/FortniteGame.FortPlayerControllerOutpost.ClientOnStructureLevelChanged"), &changed);
    loadout::SaveStructure(pc, structure, before + 1, accountItems);
    LOG("gameplay: %s upgraded %s to level %d (paid%s)", engine::Name(pc).c_str(), engine::Name(structure).c_str(), before + 1,
        spent.empty() ? " nothing" : spent.c_str());
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
    bool deploy = engine::PatchCall(kBuildCall, kBuild, reinterpret_cast<void*>(&BuildStormshield));
    LOG("gameplay: an outpost's storm shield %s", deploy ? "deploys when used" : "NOT patched (stays unbuilt)");
    bool feedback = engine::PatchCall(kFeedbackSoundCall, kSpawnSoundAttached, reinterpret_cast<void*>(&FeedbackSound));
    LOG("gameplay: building and editing sounds %s", feedback ? "play at the camera" : "NOT patched (they stay where the zone started)");
    bool drops = engine::PatchCall(kSpawnPickupInWorldCall, kSpawnPickupInWorld, reinterpret_cast<void*>(&SpawnPickupInWorld));
    LOG("gameplay: blueprint item drops (supply crates) %s", drops ? "spawn pickups" : "NOT patched (drop nothing)");
    int upgrades = 0;
    for (uint32_t site : kUpgradeStructureCalls)
        upgrades += engine::PatchCall(site, kUpgradeStructure, reinterpret_cast<void*>(&UpgradeStructure));
    LOG("gameplay: %d of %zu outpost upgrade calls build on the host", upgrades, std::size(kUpgradeStructureCalls));
}

}  // namespace gameplay
